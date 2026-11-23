#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "client_hello.h"
#include "config.h"
#include "forward.h"
#include "http_parse.h"
#include "ja4.h"
#include "rate_limit.h"
#include "tls.h"
#include "verdict.h"

/* A TLS record header (5 bytes) and the largest record body. */
#define HELLO_PEEK_MAX (5 + 16384)
/* Seconds that a client has for each read and write. */
#define CLIENT_TIMEOUT 5
#define DEFAULT_ORIGIN_TIMEOUT 5
#define DEFAULT_BURST 10.0
#define DEFAULT_TARPIT 10
#define RATE_LIMIT_KEY_MAX 4096
/* Room for the headers that rproxy adds to a request for an Origin. */
#define FORWARD_EXTRA_MAX 512
#define REPLY_TEXT_MAX 512
#define RETRY_LINE_MAX 64
#define STATUS_TEXT_MAX 8
#define LOG_NUMBER_MAX 16
#define STATUS_DIGITS 3
/* "HTTP/1.1 " comes before the status code of a reply. */
#define STATUS_OFFSET 9
#define CONN_CLOSED (-2)
#define NANOSECONDS_PER_SECOND 1e9

struct rproxy_config {
    int port;
    const char *cert_file;
    const char *key_file;
    int origin_timeout;
    int workers;
    double rate;
    double burst;
    int tarpit;
};

/* What all Workers share. */
struct rproxy {
    struct rproxy_config config;
    struct origin_list list;
    struct tls_context *context;
    struct rate_limit *limit; /* NULL when the rate limit is off */
};

struct pool {
    int fds[POOL_QUEUE_MAX];
    int head;
    int count;
    bool closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct rproxy *rproxy;
};

/* The facts of one connection, for the log line. */
struct exchange {
    char client_ip[INET6_ADDRSTRLEN];
    char ja4[JA4_LEN + 1];
    const char *verdict_name;
    int origin_index; /* -1 if no Origin was picked */
    int status;       /* 0 if no reply was sent */
    bool needs_drain;
};

struct request_buffer {
    char *data;
    size_t used;
    size_t capacity;
};

static volatile sig_atomic_t stop_requested = 0;

static bool rproxy_init_from_args(struct rproxy *rproxy, int argc, char **argv);
static bool option_parse(struct rproxy *rproxy, const char *name,
                         const char *value);
static bool number_parse(const char *text, long min, long max, int *number);
static bool real_parse(const char *text, double min, double *number);
static bool origin_add(struct origin_list *list, const char *text);
static bool rproxy_start(struct rproxy *rproxy);
static void rproxy_stop(struct rproxy *rproxy);
static void signals_install(void);
static void stop_signal_handle(int signal_number);
static int listen_socket_open(int port);
static int pool_run(int listen_fd, struct rproxy *rproxy);
static void pool_start_workers(struct pool *pool, pthread_t *workers,
                               int count);
static void pool_push(struct pool *pool, int conn_fd);
static void pool_close(struct pool *pool);
static int pool_pop(struct pool *pool);
static void *worker_main(void *arg);
static void connection_serve(struct rproxy *rproxy, int conn_fd);
static void connection_set_timeouts(int conn_fd);
static void client_ip_get(int conn_fd, char *client_ip, size_t size);
static enum client_hello_status hello_peek(int conn_fd,
                                           struct client_hello *hello);
static bool socket_peek_more(int conn_fd, unsigned char *bytes, size_t *len);
static void exchange_run(struct rproxy *rproxy, struct tls_session *session,
                         struct exchange *exchange);
static int request_read(struct tls_session *session,
                        struct request_buffer *buffer, struct request *request);
static bool buffer_reserve(struct request_buffer *buffer, size_t capacity);
static bool buffer_read_some(struct tls_session *session,
                             struct request_buffer *buffer);
static void reply_send_status(struct tls_session *session, int status,
                              int retry_after, struct exchange *exchange);
static const char *status_get_reason(int status);
static struct verdict request_decide(struct rproxy *rproxy,
                                     const struct request *request,
                                     const struct exchange *exchange);
static double clock_get_seconds(void);
static void verdict_apply(struct rproxy *rproxy, struct tls_session *session,
                          const struct request *request,
                          const struct verdict *verdict,
                          struct exchange *exchange);
static void origin_forward(struct rproxy *rproxy, struct tls_session *session,
                           const struct request *request,
                           struct exchange *exchange);
static int reply_get_status(const char *reply, size_t reply_len);
static void tarpit_hold(int seconds);
static void connection_drain(int conn_fd);
static void exchange_log(const struct exchange *exchange);

int main(int argc, char **argv)
{
    struct rproxy rproxy;
    if (!rproxy_init_from_args(&rproxy, argc, argv)) {
        fprintf(stderr,
                "usage: ./rproxy --port P --cert FILE --key FILE "
                "--origin HOST:PORT [--origin HOST:PORT ...] "
                "[--origin-timeout S] [--workers N] [--rate R] [--burst B] "
                "[--tarpit S]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signals_install();
    if (!rproxy_start(&rproxy)) {
        rproxy_stop(&rproxy);
        return 1;
    }

    int listen_fd = listen_socket_open(rproxy.config.port);
    int result = pool_run(listen_fd, &rproxy);
    close(listen_fd);
    rproxy_stop(&rproxy);

    return result == 0 ? 0 : 1;
}

static bool rproxy_init_from_args(struct rproxy *rproxy, int argc, char **argv)
{
    *rproxy = (struct rproxy){
        .config = {.origin_timeout = DEFAULT_ORIGIN_TIMEOUT,
                   .workers = DEFAULT_WORKERS,
                   .burst = DEFAULT_BURST,
                   .tarpit = DEFAULT_TARPIT},
    };
    atomic_init(&rproxy->list.next, 0);
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc || !option_parse(rproxy, argv[i], argv[i + 1])) {
            return false;
        }
    }
    const struct rproxy_config *config = &rproxy->config;

    return config->port > 0 && config->cert_file != NULL &&
           config->key_file != NULL && rproxy->list.count > 0;
}

static bool option_parse(struct rproxy *rproxy, const char *name,
                         const char *value)
{
    struct rproxy_config *config = &rproxy->config;
    if (strcmp(name, "--port") == 0) {
        return number_parse(value, 1, PORT_MAX, &config->port);
    }
    if (strcmp(name, "--cert") == 0) {
        config->cert_file = value;
        return true;
    }
    if (strcmp(name, "--key") == 0) {
        config->key_file = value;
        return true;
    }
    if (strcmp(name, "--origin") == 0) {
        return origin_add(&rproxy->list, value);
    }
    if (strcmp(name, "--origin-timeout") == 0) {
        return number_parse(value, 1, TIMEOUT_MAX, &config->origin_timeout);
    }
    if (strcmp(name, "--workers") == 0) {
        return number_parse(value, 1, WORKERS_MAX, &config->workers);
    }
    if (strcmp(name, "--rate") == 0) {
        return real_parse(value, 0, &config->rate);
    }
    if (strcmp(name, "--burst") == 0) {
        return real_parse(value, 1, &config->burst);
    }
    if (strcmp(name, "--tarpit") == 0) {
        return number_parse(value, 0, TIMEOUT_MAX, &config->tarpit);
    }

    return false;
}

static bool number_parse(const char *text, long min, long max, int *number)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min ||
        value > max) {
        return false;
    }
    *number = (int)value;

    return true;
}

static bool real_parse(const char *text, double min, double *number)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !(value >= min)) {
        return false;
    }
    *number = value;

    return true;
}

static bool origin_add(struct origin_list *list, const char *text)
{
    if (list->count == ORIGIN_MAX ||
        origin_parse(text, &list->origins[list->count]) != 0) {
        return false;
    }
    list->count++;

    return true;
}

/* Loads the certificate and makes the rate limit table. */
static bool rproxy_start(struct rproxy *rproxy)
{
    rproxy->context =
        tls_context_create(rproxy->config.cert_file, rproxy->config.key_file);
    if (rproxy->context == NULL) {
        fprintf(stderr, "rproxy: cannot load %s and %s\n",
                rproxy->config.cert_file, rproxy->config.key_file);
        return false;
    }
    if (rproxy->config.rate > 0) {
        struct rate_limit_options options = {.rate = rproxy->config.rate,
                                             .burst = rproxy->config.burst,
                                             .key_max = RATE_LIMIT_KEY_MAX};
        rproxy->limit = rate_limit_create(&options);
        if (rproxy->limit == NULL) {
            fprintf(stderr, "rproxy: rate_limit_create failed\n");
            return false;
        }
    }

    return true;
}

static void rproxy_stop(struct rproxy *rproxy)
{
    if (rproxy->limit != NULL) {
        rate_limit_destroy(rproxy->limit);
    }
    tls_context_destroy(rproxy->context);
}

/* No SA_RESTART, so accept returns EINTR and the loop sees the flag. */
static void signals_install(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_signal_handle;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

static void stop_signal_handle(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int listen_socket_open(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons((uint16_t)port)};
    if (listen_fd < 0 ||
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
            0 ||
        bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, SOMAXCONN) < 0) {
        perror("rproxy: listen socket");
        exit(EXIT_FAILURE);
    }
    printf("listening on port %d\n", port);

    return listen_fd;
}

/* The pool Concurrency model, as in src/model_pool.c. */
static int pool_run(int listen_fd, struct rproxy *rproxy)
{
    struct pool pool = {.rproxy = rproxy};
    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.not_empty, NULL);
    pthread_cond_init(&pool.not_full, NULL);
    int count = rproxy->config.workers;
    pthread_t *workers = calloc((size_t)count, sizeof(*workers));
    if (workers == NULL) {
        return -1;
    }
    pool_start_workers(&pool, workers, count);

    int result = 0;
    while (!stop_requested) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd >= 0) {
            pool_push(&pool, conn_fd);
        } else if (errno != EINTR) {
            result = -1;
            break;
        }
    }
    pool_close(&pool);
    for (int i = 0; i < count; i++) {
        pthread_join(workers[i], NULL);
    }
    free(workers);

    return result;
}

/* Workers inherit the mask, so only the main thread gets the signals. */
static void pool_start_workers(struct pool *pool, pthread_t *workers, int count)
{
    sigset_t blocked;
    sigset_t old;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked, &old);
    for (int i = 0; i < count; i++) {
        pthread_create(&workers[i], NULL, worker_main, pool);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void pool_push(struct pool *pool, int conn_fd)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == POOL_QUEUE_MAX) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }
    pool->fds[(pool->head + pool->count) % POOL_QUEUE_MAX] = conn_fd;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
}

static void pool_close(struct pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    pool->closed = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
}

/* Returns -1 when the pool is closed and the queue is empty. */
static int pool_pop(struct pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == 0 && !pool->closed) {
        pthread_cond_wait(&pool->not_empty, &pool->lock);
    }
    int conn_fd = -1;
    if (pool->count > 0) {
        conn_fd = pool->fds[pool->head];
        pool->head = (pool->head + 1) % POOL_QUEUE_MAX;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
    }
    pthread_mutex_unlock(&pool->lock);

    return conn_fd;
}

static void *worker_main(void *arg)
{
    struct pool *pool = arg;
    for (;;) {
        int conn_fd = pool_pop(pool);
        if (conn_fd < 0) {
            return NULL;
        }
        connection_serve(pool->rproxy, conn_fd);
        close(conn_fd);
    }
}

/* SPEC 11: ClientHello and JA4, TLS handshake, request, rate limit,
   Verdict, Origin. One request for each connection. */
static void connection_serve(struct rproxy *rproxy, int conn_fd)
{
    struct exchange exchange = {.verdict_name = "-", .origin_index = -1};
    struct client_hello hello;
    connection_set_timeouts(conn_fd);
    client_ip_get(conn_fd, exchange.client_ip, sizeof(exchange.client_ip));
    if (hello_peek(conn_fd, &hello) != CLIENT_HELLO_OK) {
        exchange.verdict_name = "no-tls";
        exchange_log(&exchange);
        return;
    }
    ja4_format(&hello, exchange.ja4);

    struct tls_session *session = tls_session_create(rproxy->context, conn_fd);
    if (session == NULL) {
        exchange.verdict_name = "no-handshake";
        exchange_log(&exchange);
        return;
    }
    exchange_run(rproxy, session, &exchange);
    tls_session_destroy(session);
    if (exchange.needs_drain) {
        connection_drain(conn_fd);
    }
    exchange_log(&exchange);
}

static void connection_set_timeouts(int conn_fd)
{
    struct timeval limit = {.tv_sec = CLIENT_TIMEOUT, .tv_usec = 0};
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

static void client_ip_get(int conn_fd, char *client_ip, size_t size)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    const void *host = NULL;
    snprintf(client_ip, size, "-");
    if (getpeername(conn_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return;
    }
    if (addr.ss_family == AF_INET) {
        host = &((struct sockaddr_in *)&addr)->sin_addr;
    } else if (addr.ss_family == AF_INET6) {
        host = &((struct sockaddr_in6 *)&addr)->sin6_addr;
    }
    if (host != NULL) {
        inet_ntop(addr.ss_family, host, client_ip, (socklen_t)size);
    }
}

/* Reads the ClientHello with MSG_PEEK, so that OpenSSL still reads it
   for the handshake. */
static enum client_hello_status hello_peek(int conn_fd,
                                           struct client_hello *hello)
{
    unsigned char bytes[HELLO_PEEK_MAX];
    size_t len = 0;
    enum client_hello_status status = CLIENT_HELLO_INCOMPLETE;
    while (status == CLIENT_HELLO_INCOMPLETE) {
        if (!socket_peek_more(conn_fd, bytes, &len)) {
            return CLIENT_HELLO_INVALID;
        }
        status = client_hello_parse(bytes, len, hello);
    }

    return status;
}

/* A peek returns at once while the bytes that are there stay. So wait
   until at least len + 1 bytes are there (MSG_WAITALL), then peek all of
   them. Returns false on the end, a timeout, or a full buffer. */
static bool socket_peek_more(int conn_fd, unsigned char *bytes, size_t *len)
{
    if (*len >= HELLO_PEEK_MAX) {
        return false;
    }
    ssize_t count = recv(conn_fd, bytes, *len + 1, MSG_PEEK | MSG_WAITALL);
    if (count < (ssize_t)(*len + 1)) {
        return false;
    }
    count = recv(conn_fd, bytes, HELLO_PEEK_MAX, MSG_PEEK);
    if (count <= (ssize_t)*len) {
        return false;
    }
    *len = (size_t)count;

    return true;
}

static void exchange_run(struct rproxy *rproxy, struct tls_session *session,
                         struct exchange *exchange)
{
    struct request_buffer buffer = {0};
    struct request request;
    int status = request_read(session, &buffer, &request);
    if (status == 0) {
        struct verdict verdict = request_decide(rproxy, &request, exchange);
        verdict_apply(rproxy, session, &request, &verdict, exchange);
    } else if (status != CONN_CLOSED) {
        reply_send_status(session, status, 0, exchange);
        exchange->needs_drain = true;
    }
    free(buffer.data);
}

/* Returns 0 with request set, a status code of the Shared core, or
   CONN_CLOSED (the end, a timeout, or an error). */
static int request_read(struct tls_session *session,
                        struct request_buffer *buffer, struct request *request)
{
    long frame = HTTP_INCOMPLETE;
    while (frame == HTTP_INCOMPLETE || buffer->used < (size_t)frame) {
        size_t want = frame == HTTP_INCOMPLETE ? HEADER_MAX : (size_t)frame;
        if (!buffer_reserve(buffer, want)) {
            return 500;
        }
        if (!buffer_read_some(session, buffer)) {
            return CONN_CLOSED;
        }
        frame = http_frame_measure(buffer->data, buffer->used);
        if (frame < HTTP_INCOMPLETE) {
            return (int)-frame;
        }
    }

    return http_parse_request(buffer->data, (size_t)frame, request);
}

static bool buffer_reserve(struct request_buffer *buffer, size_t capacity)
{
    if (buffer->capacity >= capacity) {
        return true;
    }
    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;

    return true;
}

static bool buffer_read_some(struct tls_session *session,
                             struct request_buffer *buffer)
{
    size_t count = tls_session_read(session, buffer->data + buffer->used,
                                    buffer->capacity - buffer->used);
    buffer->used += count;

    return count > 0;
}

/* A short text reply of rproxy itself. retry_after > 0 adds Retry-After. */
static void reply_send_status(struct tls_session *session, int status,
                              int retry_after, struct exchange *exchange)
{
    char retry_line[RETRY_LINE_MAX] = "";
    char body[REPLY_TEXT_MAX];
    char text[REPLY_TEXT_MAX * 2];
    if (retry_after > 0) {
        snprintf(retry_line, sizeof(retry_line), "Retry-After: %d\r\n",
                 retry_after);
    }
    const char *reason = status_get_reason(status);
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    int len = snprintf(text, sizeof(text),
                       "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                       "Content-Length: %d\r\n%sConnection: close\r\n\r\n%s",
                       status, reason, body_len, retry_line, body);
    tls_session_send(session, text, (size_t)len);
    exchange->status = status;
}

static const char *status_get_reason(int status)
{
    switch (status) {
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 502:
        return "Bad Gateway";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

static struct verdict request_decide(struct rproxy *rproxy,
                                     const struct request *request,
                                     const struct exchange *exchange)
{
    struct verdict_signals signals = {.ja4 = exchange->ja4,
                                      .user_agent = request->user_agent,
                                      .target = request->target};
    if (rproxy->limit != NULL) {
        signals.retry_after = rate_limit_take(
            rproxy->limit, exchange->client_ip, clock_get_seconds());
    }

    return verdict_decide(&signals);
}

static double clock_get_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (double)now.tv_sec + (double)now.tv_nsec / NANOSECONDS_PER_SECOND;
}

static void verdict_apply(struct rproxy *rproxy, struct tls_session *session,
                          const struct request *request,
                          const struct verdict *verdict,
                          struct exchange *exchange)
{
    switch (verdict->kind) {
    case VERDICT_PASS:
        exchange->verdict_name = "pass";
        origin_forward(rproxy, session, request, exchange);
        break;
    case VERDICT_DENY:
        exchange->verdict_name = "deny";
        reply_send_status(session, 403, 0, exchange);
        break;
    case VERDICT_SLOW:
        exchange->verdict_name = "slow";
        reply_send_status(session, 429, verdict->retry_after, exchange);
        break;
    case VERDICT_TARPIT:
        exchange->verdict_name = "tarpit";
        tarpit_hold(rproxy->config.tarpit);
        break;
    }
}

static void origin_forward(struct rproxy *rproxy, struct tls_session *session,
                           const struct request *request,
                           struct exchange *exchange)
{
    size_t size = HEADER_MAX + FORWARD_EXTRA_MAX + request->body_len;
    char *request_bytes = malloc(size);
    size_t request_len =
        request_bytes == NULL
            ? 0
            : forward_format_request(request, exchange->client_ip,
                                     exchange->ja4, request_bytes, size);
    if (request_len == 0) {
        free(request_bytes);
        reply_send_status(session, 500, 0, exchange);
        return;
    }
    char *reply = NULL;
    size_t reply_len = 0;
    size_t first = origin_list_pick(&rproxy->list);
    exchange->origin_index = (int)first;
    int status =
        forward_fetch(&rproxy->list, first, request_bytes, request_len,
                      rproxy->config.origin_timeout, &reply, &reply_len);
    free(request_bytes);
    if (status != 0) {
        reply_send_status(session, status, 0, exchange);
        return;
    }
    tls_session_send(session, reply, reply_len);
    exchange->status = reply_get_status(reply, reply_len);
    free(reply);
}

/* Returns the status code of an HTTP reply, or 0 if it has none. */
static int reply_get_status(const char *reply, size_t reply_len)
{
    if (reply_len < STATUS_OFFSET + STATUS_DIGITS ||
        strncmp(reply, "HTTP/", strlen("HTTP/")) != 0) {
        return 0;
    }
    char text[STATUS_TEXT_MAX] = "";
    memcpy(text, reply + STATUS_OFFSET, STATUS_DIGITS);

    return atoi(text);
}

/* Holds the Worker one second at a time, so that a stop does not wait. */
static void tarpit_hold(int seconds)
{
    for (int i = 0; i < seconds && !stop_requested; i++) {
        sleep(1);
    }
}

/* After an error reply, unread request bytes make close() send RST, and
   the client can lose the reply. Stop writing, then read and drop the
   rest for a short time, so that close() sends FIN. */
static void connection_drain(int conn_fd)
{
    char scrap[DRAIN_CHUNK_SIZE];
    struct pollfd watch = {.fd = conn_fd, .events = POLLIN};
    shutdown(conn_fd, SHUT_WR);
    for (int i = 0; i < DRAIN_READS_MAX; i++) {
        if (poll(&watch, 1, DRAIN_WAIT_MS) <= 0 ||
            read(conn_fd, scrap, sizeof(scrap)) <= 0) {
            return;
        }
    }
}

/* One line for each connection: client IP, JA4, Verdict, Origin index,
   status. "-" marks a value that the connection did not reach. */
static void exchange_log(const struct exchange *exchange)
{
    char origin_text[LOG_NUMBER_MAX] = "-";
    char status_text[LOG_NUMBER_MAX] = "-";
    if (exchange->origin_index >= 0) {
        snprintf(origin_text, sizeof(origin_text), "%d",
                 exchange->origin_index);
    }
    if (exchange->status > 0) {
        snprintf(status_text, sizeof(status_text), "%d", exchange->status);
    }
    fprintf(stderr, "%s %s %s %s %s\n", exchange->client_ip,
            exchange->ja4[0] != '\0' ? exchange->ja4 : "-",
            exchange->verdict_name, origin_text, status_text);
}
