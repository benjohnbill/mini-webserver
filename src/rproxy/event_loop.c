/* SO_REUSEPORT is a Linux socket option, not POSIX. glibc shows it only
   with _DEFAULT_SOURCE, which -D_POSIX_C_SOURCE turns off. */
#define _DEFAULT_SOURCE /* NOLINT(readability-identifier-naming) */

#include "event_loop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "behavior.h"
#include "block_table.h"
#include "bot_auth.h"
#include "cache.h"
#include "client_hello.h"
#include "config.h"
#include "connection_verdict.h"
#include "forward.h"
#include "http_parse.h"
#include "ip_list.h"
#include "ja4.h"
#include "ja4_table.h"
#include "pow.h"
#include "rate_limit.h"
#include "rproxy.h"
#include "timer_heap.h"
#include "tls.h"
#include "verdict.h"

/* A TLS record header (5 bytes) and the largest record body. */
#define HELLO_PEEK_MAX (5 + 16384)
/* Seconds that a client has for each step that waits for it. The
   deadline moves when bytes move. */
#define CLIENT_TIMEOUT 5
#define MS_PER_SECOND 1000
/* Seconds that the drain after an error reply can take. */
#define DRAIN_TIMEOUT ((double)DRAIN_WAIT_MS / MS_PER_SECOND)
/* The value of crawler-price in a 402: the example of Cloudflare pay per
   crawl. rproxy takes no payment. */
#define PAY_PRICE "USD 0.01"
/* Room for the headers that rproxy adds to a request for an Origin. */
#define FORWARD_EXTRA_MAX 512
#define REPLY_TEXT_MAX 512
#define HEADER_LINES_MAX 256
#define STATUS_TEXT_MAX 8
#define LOG_NUMBER_MAX 16
#define STATUS_DIGITS 3
/* "HTTP/1.1 " comes before the status code of a reply. */
#define STATUS_OFFSET 9
/* The status line, X-Cache, and the rest of a reply. */
#define REPLY_PARTS_MAX 3
#define NANOSECONDS_PER_SECOND 1e9

/* The steps of one connection, in their order. One request for each
   connection (SPEC 11 "Two Verdict stages"). */
enum connection_state {
    STATE_HELLO,     /* peek the ClientHello, then the connection Verdict */
    STATE_TARPIT,    /* hold the socket and read nothing until the timer */
    STATE_HANDSHAKE, /* TLS */
    STATE_REQUEST,   /* read the request, then the request Verdict */
    STATE_ORIGIN_CONNECT,
    STATE_ORIGIN_SEND,
    STATE_ORIGIN_READ,
    STATE_RELAY, /* backpressure: read the Origin and send in turns */
    STATE_REPLY, /* send the reply to the client */
    STATE_DRAIN, /* read and drop the rest after an error reply */
    STATE_CLOSED /* freed at the end of the loop turn */
};

/* What a step tells the loop: run the next state now, wait for an event
   or the timer, or close the connection. */
enum step { STEP_NEXT, STEP_WAIT, STEP_CLOSE };

/* epoll data.ptr of a client socket or of an Origin socket. The listen
   socket has NULL. */
struct endpoint {
    struct connection *connection;
    bool is_origin;
};

/* The facts of one connection, for the log line. */
struct exchange {
    char client_ip[INET6_ADDRSTRLEN];
    char ja4[JA4_LEN + 1];
    const char *ja4_label; /* from the JA4 table: NULL if not there */
    const char *verdict_name;
    const char *cache_name; /* "hit", "miss", or "-" */
    int origin_index;       /* -1 if no Origin was picked */
    int status;             /* 0 if no reply was sent */
    size_t reply_bytes;     /* the reply to the client, without X-Cache */
    bool needs_drain;
    bool has_behavior; /* the request stage counted this request */
    struct behavior_counts behavior;
};

struct request_buffer {
    char *data;
    size_t used;
    size_t capacity;
};

/* RELAY_BACKPRESSURE: the bytes from the Origin that the client did not
   take yet. data[sent] to data[used] waits for the client, and data[used]
   to data[capacity] is the room for the Origin. */
struct relay_buffer {
    char *data;
    size_t capacity;
    size_t used;
    size_t sent;
    bool origin_is_done; /* the Origin sent EOF */
};

struct reply_part {
    const char *bytes;
    size_t len;
};

struct connection {
    struct event_loop *loop;
    enum connection_state state;
    int conn_fd;
    int origin_fd; /* -1 while no Origin socket is open */
    struct endpoint client_end;
    struct endpoint origin_end;
    uint32_t client_events; /* what conn_fd waits for: EPOLLIN or EPOLLOUT */
    uint32_t origin_events; /* what origin_fd waits for: 0 is nothing */
    struct timer timer;     /* the deadline of the current state */
    size_t hello_len;       /* the bytes of the last ClientHello peek */
    /* NULL before the handshake, and after the close_notify */
    struct tls_session *session;
    struct request_buffer buffer;
    long frame; /* http_frame_measure of the buffer */
    struct request request;
    bool is_decided;      /* the request got a request Verdict */
    char *origin_request; /* the bytes for the Origin */
    size_t origin_request_len;
    size_t origin_request_sent;
    size_t origin_index;  /* the Origin of the current connect */
    size_t address_index; /* its address of the current connect */
    bool has_tried_next;
    char *reply; /* from the Origin or the cache */
    size_t reply_len;
    size_t reply_capacity;
    struct relay_buffer relay;
    bool holds_exchange_slot;             /* RELAY_CONN_CAP: until the close */
    char status_text[REPLY_TEXT_MAX * 2]; /* a reply of rproxy itself */
    struct reply_part parts[REPLY_PARTS_MAX];
    int part_count;
    int part_index;
    size_t part_offset;
    int drain_reads;
    struct exchange exchange;
    struct connection *prev;
    struct connection *next;
};

struct event_loop {
    struct rproxy *rproxy;
    int epoll_fd;
    int listen_fd;
    struct timer_heap *timers;
    struct connection *connections; /* the open connections */
    struct connection *closed;      /* freed at the end of the loop turn */
    int connection_count;
    unsigned char scratch[HELLO_PEEK_MAX]; /* for the peek and the drain */
};

static int listen_socket_open(int port);
static int loop_get_wait_ms(const struct event_loop *loop);
static double clock_get_seconds(void);
static void loop_dispatch(struct event_loop *loop,
                          const struct epoll_event *event);
static void loop_accept(struct event_loop *loop);
static void connection_refuse(int conn_fd);
static void client_ip_get(int conn_fd, char *client_ip, size_t size);
static void connection_open(struct event_loop *loop, int conn_fd);
static void connection_set_deadline(struct connection *connection,
                                    double seconds);
static void connection_advance(struct connection *connection);
static enum step connection_step(struct connection *connection);
static enum step hello_step(struct connection *connection);
static enum client_hello_status hello_peek(struct connection *connection,
                                           struct client_hello *hello);
static const char *ja4_label_find(const struct rproxy *rproxy, const char *ja4);
static enum step connection_verdict_apply(struct connection *connection);
static enum connection_verdict
connection_verdict_get(struct rproxy *rproxy, const struct exchange *exchange);
static enum step handshake_start(struct connection *connection);
static enum step tarpit_start(struct connection *connection,
                              const char *verdict_name);
static enum step handshake_step(struct connection *connection);
static enum step connection_wait_tls(struct connection *connection,
                                     enum tls_status status);
static void connection_watch_client(struct connection *connection,
                                    uint32_t events);
static enum step request_step(struct connection *connection);
static bool buffer_reserve(struct request_buffer *buffer, size_t capacity);
static enum tls_status buffer_read_some(struct connection *connection);
static enum step request_fail(struct connection *connection, int status);
static void status_reply_plan(struct connection *connection, int status,
                              const char *header_lines);
static const char *status_get_reason(int status);
static enum step reply_start(struct connection *connection);
static enum step request_verdict_apply(struct connection *connection);
static struct verdict request_decide(struct rproxy *rproxy,
                                     const struct request *request,
                                     struct exchange *exchange);
static bool target_has_prefix(const char *target, const char *const *prefixes,
                              int count);
static bool request_has_pow(const struct rproxy *rproxy,
                            const struct request *request,
                            const struct exchange *exchange);
static double wall_clock_get_seconds(void);
static bool request_is_signed(const struct rproxy *rproxy,
                              const struct request *request);
static const char *request_find_header(const void *context, const char *name);
static void client_block(struct rproxy *rproxy,
                         const struct exchange *exchange);
static void challenge_plan(struct connection *connection);
static enum step request_forward(struct connection *connection);
static bool cache_key_format(const struct request *request, char *key,
                             size_t size);
static void reply_plan_origin(struct connection *connection);
static bool reply_is_not_modified(const struct request *request,
                                  const char *reply, size_t reply_len);
static int reply_get_status(const char *reply, size_t reply_len);
static bool not_modified_plan(struct connection *connection);
static void reply_plan_parts(struct connection *connection,
                             const char *cache_name);
static enum step origin_exchange_start(struct connection *connection);
static int exchange_slot_take(struct connection *connection);
static void client_send_buffer_cap(struct connection *connection);
static bool origin_request_format(struct connection *connection);
static enum step origin_connect_start(struct connection *connection);
static int origin_get_receive_buffer(const struct rproxy *rproxy);
static enum step origin_connect_retry(struct connection *connection);
static void origin_socket_close(struct connection *connection);
static enum step origin_exchange_end(struct connection *connection, int status);
static enum step origin_connect_step(struct connection *connection);
static enum step origin_send_step(struct connection *connection);
static void connection_watch_origin(struct connection *connection,
                                    uint32_t events);
static enum step relay_start(struct connection *connection);
static enum step relay_step(struct connection *connection);
static bool relay_read_origin(struct connection *connection, bool *has_moved);
static void relay_compact(struct relay_buffer *relay);
static void relay_end_origin(struct connection *connection);
static enum tls_status relay_send_client(struct connection *connection,
                                         bool *has_moved);
static enum step relay_wait(struct connection *connection,
                            enum tls_status status, bool has_moved);
static enum step origin_read_step(struct connection *connection);
static bool reply_is_over_cap(const struct connection *connection);
static bool reply_reserve(struct connection *connection);
static enum step reply_step(struct connection *connection);
static void reply_advance(struct connection *connection, size_t count);
static enum step reply_end(struct connection *connection);
static enum step drain_step(struct connection *connection);
static void connection_close(struct connection *connection);
static void exchange_log(const struct exchange *exchange);
static void behavior_log(const struct exchange *exchange);
static void loop_expire_timers(struct event_loop *loop);
static void connection_expire(struct connection *connection);
static void relay_expire(struct connection *connection);
static void exchange_slot_release(struct connection *connection);
static void loop_free_closed(struct event_loop *loop);
static void loop_close_all(struct event_loop *loop);

struct event_loop *event_loop_create(struct rproxy *rproxy)
{
    struct event_loop *loop = calloc(1, sizeof(*loop));
    if (loop == NULL) {
        perror("rproxy: event loop");
        return NULL;
    }
    loop->rproxy = rproxy;
    loop->listen_fd = listen_socket_open(rproxy->config.port);
    loop->epoll_fd = epoll_create1(0);
    loop->timers = timer_heap_create((size_t)rproxy->config.connections);
    struct epoll_event event = {.events = EPOLLIN, .data.ptr = NULL};
    if (loop->listen_fd < 0 || loop->epoll_fd < 0 || loop->timers == NULL ||
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->listen_fd, &event) !=
            0) {
        perror("rproxy: event loop");
        event_loop_destroy(loop);
        return NULL;
    }

    return loop;
}

void event_loop_destroy(struct event_loop *loop)
{
    if (loop == NULL) {
        return;
    }
    if (loop->listen_fd >= 0) {
        close(loop->listen_fd);
    }
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
    }
    timer_heap_destroy(loop->timers);
    free(loop);
}

/* One turn: wait for events until the first deadline, run the
   connections that are ready, then the connections whose timer ended. */
int event_loop_run(struct event_loop *loop)
{
    struct epoll_event events[EPOLL_EVENTS_MAX];
    int result = 0;
    while (!atomic_load(&loop->rproxy->is_stopping)) {
        int count = epoll_wait(loop->epoll_fd, events, EPOLL_EVENTS_MAX,
                               loop_get_wait_ms(loop));
        if (count < 0 && errno != EINTR) {
            perror("rproxy: epoll_wait");
            result = -1;
            break;
        }
        for (int i = 0; i < count; i++) {
            loop_dispatch(loop, &events[i]);
        }
        loop_expire_timers(loop);
        loop_free_closed(loop);
    }
    loop_close_all(loop);

    return result;
}

bool event_loop_is_port_free(int port)
{
    int probe_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (probe_fd < 0) {
        return false;
    }
    int reuse = 1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons((uint16_t)port)};
    setsockopt(probe_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bool is_free = bind(probe_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(probe_fd);

    return is_free;
}

/* SO_REUSEPORT: each loop has its own listen socket on the same port, and
   the kernel gives each new connection to one of them. */
static int listen_socket_open(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        return -1;
    }
    int reuse = 1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons((uint16_t)port)};
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
            0 ||
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) <
            0 ||
        bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, SOMAXCONN) < 0) {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

/* Until the first deadline, but at most EPOLL_WAIT_MS, so that the loop
   sees a stop within 1 s. */
static int loop_get_wait_ms(const struct event_loop *loop)
{
    const struct timer *first = timer_heap_find_first(loop->timers);
    if (first == NULL) {
        return EPOLL_WAIT_MS;
    }
    double wait = (first->deadline - clock_get_seconds()) * MS_PER_SECOND;
    if (wait <= 0) {
        return 0;
    }

    return wait >= EPOLL_WAIT_MS ? EPOLL_WAIT_MS : (int)wait + 1;
}

static double clock_get_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (double)now.tv_sec + (double)now.tv_nsec / NANOSECONDS_PER_SECOND;
}

/* An event of the client socket runs only a client state, and an event of
   the Origin socket only an Origin state. A tarpit runs nothing. */
static void loop_dispatch(struct event_loop *loop,
                          const struct epoll_event *event)
{
    const struct endpoint *end = event->data.ptr;
    if (end == NULL) {
        loop_accept(loop);
        return;
    }
    struct connection *connection = end->connection;
    enum connection_state state = connection->state;
    bool is_at_origin = state == STATE_ORIGIN_CONNECT ||
                        state == STATE_ORIGIN_SEND ||
                        state == STATE_ORIGIN_READ;
    /* A relay waits for both sockets. */
    if (state == STATE_RELAY ||
        (state != STATE_CLOSED && end->is_origin == is_at_origin)) {
        connection_advance(connection);
    }
}

/* Accepts until the queue is empty. Over --connections, a new connection
   closes at once. */
static void loop_accept(struct event_loop *loop)
{
    for (;;) {
        int conn_fd = accept(loop->listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            return;
        }
        if (loop->connection_count >= loop->rproxy->config.connections) {
            connection_refuse(conn_fd);
            continue;
        }
        connection_open(loop, conn_fd);
    }
}

static void connection_refuse(int conn_fd)
{
    struct exchange exchange = {
        .verdict_name = "full", .cache_name = "-", .origin_index = -1};
    client_ip_get(conn_fd, exchange.client_ip, sizeof(exchange.client_ip));
    close(conn_fd);
    exchange_log(&exchange);
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

/* Edge-triggered: a peek leaves the ClientHello in the socket, so a
   level-triggered EPOLLIN would wake the loop again and again. Each step
   reads or writes until the socket says EAGAIN. */
static void connection_open(struct event_loop *loop, int conn_fd)
{
    struct connection *connection = calloc(1, sizeof(*connection));
    if (connection == NULL) {
        close(conn_fd);
        return;
    }
    *connection = (struct connection){
        .loop = loop,
        .state = STATE_HELLO,
        .conn_fd = conn_fd,
        .origin_fd = -1,
        .client_end = {.connection = connection, .is_origin = false},
        .origin_end = {.connection = connection, .is_origin = true},
        .client_events = EPOLLIN,
        .frame = HTTP_INCOMPLETE,
        .exchange = {
            .verdict_name = "-", .cache_name = "-", .origin_index = -1}};
    timer_init(&connection->timer);
    client_ip_get(conn_fd, connection->exchange.client_ip,
                  sizeof(connection->exchange.client_ip));
    fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL, 0) | O_NONBLOCK);
    struct epoll_event event = {.events = EPOLLIN | EPOLLET,
                                .data.ptr = &connection->client_end};
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) != 0) {
        close(conn_fd);
        free(connection);
        return;
    }
    connection->next = loop->connections;
    if (loop->connections != NULL) {
        loop->connections->prev = connection;
    }
    loop->connections = connection;
    loop->connection_count++;
    connection_set_deadline(connection, CLIENT_TIMEOUT);
    connection_advance(connection);
}

/* The heap has room for each connection of the loop, so the add works. */
static void connection_set_deadline(struct connection *connection,
                                    double seconds)
{
    timer_heap_add(connection->loop->timers, &connection->timer,
                   clock_get_seconds() + seconds);
}

static void connection_advance(struct connection *connection)
{
    enum step step = STEP_NEXT;
    while (step == STEP_NEXT) {
        step = connection_step(connection);
    }
    if (step == STEP_CLOSE) {
        connection_close(connection);
    }
}

static enum step connection_step(struct connection *connection)
{
    switch (connection->state) {
    case STATE_HELLO:
        return hello_step(connection);
    case STATE_HANDSHAKE:
        return handshake_step(connection);
    case STATE_REQUEST:
        return request_step(connection);
    case STATE_ORIGIN_CONNECT:
        return origin_connect_step(connection);
    case STATE_ORIGIN_SEND:
        return origin_send_step(connection);
    case STATE_ORIGIN_READ:
        return origin_read_step(connection);
    case STATE_RELAY:
        return relay_step(connection);
    case STATE_REPLY:
        return reply_step(connection);
    case STATE_DRAIN:
        return drain_step(connection);
    case STATE_TARPIT:
    case STATE_CLOSED:
        break;
    }

    return STEP_WAIT;
}

/* SPEC 11: ClientHello and JA4, then the connection Verdict, before any
   byte of TLS goes out. */
static enum step hello_step(struct connection *connection)
{
    struct exchange *exchange = &connection->exchange;
    struct client_hello hello;
    enum client_hello_status status = hello_peek(connection, &hello);
    if (status == CLIENT_HELLO_INCOMPLETE) {
        return STEP_WAIT;
    }
    if (status != CLIENT_HELLO_OK) {
        exchange->verdict_name = "no-tls";
        return STEP_CLOSE;
    }
    ja4_format(&hello, exchange->ja4);
    exchange->ja4_label =
        ja4_label_find(connection->loop->rproxy, exchange->ja4);

    return connection_verdict_apply(connection);
}

/* Reads the ClientHello with MSG_PEEK, so that OpenSSL still reads it for
   the handshake. Each peek sees all the bytes from the start. INCOMPLETE
   means: wait for more bytes. */
static enum client_hello_status hello_peek(struct connection *connection,
                                           struct client_hello *hello)
{
    unsigned char *bytes = connection->loop->scratch;
    ssize_t count = recv(connection->conn_fd, bytes, HELLO_PEEK_MAX, MSG_PEEK);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return CLIENT_HELLO_INCOMPLETE;
    }
    if (count <= 0) {
        return CLIENT_HELLO_INVALID;
    }
    enum client_hello_status status =
        client_hello_parse(bytes, (size_t)count, hello);
    if (status == CLIENT_HELLO_INCOMPLETE && count == HELLO_PEEK_MAX) {
        return CLIENT_HELLO_INVALID;
    }
    if ((size_t)count > connection->hello_len) {
        connection->hello_len = (size_t)count;
        connection_set_deadline(connection, CLIENT_TIMEOUT);
    }

    return status;
}

/* An empty JA4 (no ClientHello fingerprint) has no label. */
static const char *ja4_label_find(const struct rproxy *rproxy, const char *ja4)
{
    if (rproxy->fingerprints == NULL || ja4[0] == '\0') {
        return NULL;
    }

    return ja4_table_find(rproxy->fingerprints, ja4);
}

/* A drop closes with the ClientHello unread, so close() sends RST and no
   byte of TLS. */
static enum step connection_verdict_apply(struct connection *connection)
{
    struct exchange *exchange = &connection->exchange;
    switch (connection_verdict_get(connection->loop->rproxy, exchange)) {
    case CONN_PASS:
        break;
    case CONN_DROP:
        exchange->verdict_name = "drop";
        return STEP_CLOSE;
    case CONN_TARPIT:
        return tarpit_start(connection, "tarpit");
    }

    return handshake_start(connection);
}

/* The block table has two kinds of keys: client IPs and JA4s. An empty
   JA4 (no ClientHello fingerprint) is never a key. */
static enum connection_verdict
connection_verdict_get(struct rproxy *rproxy, const struct exchange *exchange)
{
    double now = clock_get_seconds();
    const char *ja4 = exchange->ja4;
    struct connection_signals signals = {
        .is_listed =
            rproxy->block_list != NULL &&
            ip_list_has_address(rproxy->block_list, exchange->client_ip),
        .is_blocked =
            block_table_has_key(rproxy->blocks, exchange->client_ip, now) ||
            (ja4[0] != '\0' && block_table_has_key(rproxy->blocks, ja4, now)),
        .ja4_label = exchange->ja4_label,
    };

    return connection_decide(&signals);
}

static enum step handshake_start(struct connection *connection)
{
    connection->session = tls_session_create(connection->loop->rproxy->context,
                                             connection->conn_fd);
    if (connection->session == NULL) {
        connection->exchange.verdict_name = "no-handshake";
        return STEP_CLOSE;
    }
    connection->state = STATE_HANDSHAKE;
    connection_set_deadline(connection, CLIENT_TIMEOUT);

    return STEP_NEXT;
}

/* A tarpit is a timer, not a sleep: the socket stays open, the loop reads
   nothing from it, and no thread waits. At the deadline it closes. */
static enum step tarpit_start(struct connection *connection,
                              const char *verdict_name)
{
    connection->exchange.verdict_name = verdict_name;
    connection->state = STATE_TARPIT;
    connection_set_deadline(connection,
                            connection->loop->rproxy->config.tarpit);

    return STEP_WAIT;
}

static enum step handshake_step(struct connection *connection)
{
    enum tls_status status = tls_session_handshake(connection->session);
    if (status == TLS_CLOSED) {
        connection->exchange.verdict_name = "no-handshake";
        return STEP_CLOSE;
    }
    if (status != TLS_DONE) {
        return connection_wait_tls(connection, status);
    }
    connection->state = STATE_REQUEST;
    connection_set_deadline(connection, CLIENT_TIMEOUT);

    return STEP_NEXT;
}

/* OpenSSL says which direction it waits for (SSL_get_error(3)). A read
   can want write, and a write can want read. */
static enum step connection_wait_tls(struct connection *connection,
                                     enum tls_status status)
{
    connection_watch_client(connection,
                            status == TLS_WANT_WRITE ? EPOLLOUT : EPOLLIN);

    return STEP_WAIT;
}

static void connection_watch_client(struct connection *connection,
                                    uint32_t events)
{
    if (connection->client_events == events) {
        return;
    }
    struct epoll_event event = {.events = events | EPOLLET,
                                .data.ptr = &connection->client_end};
    epoll_ctl(connection->loop->epoll_fd, EPOLL_CTL_MOD, connection->conn_fd,
              &event);
    connection->client_events = events;
}

/* Reads until the frame of http_frame_measure is in the buffer. Each
   read goes on until OpenSSL wants more bytes from the socket, because
   OpenSSL can hold bytes that epoll does not see. */
static enum step request_step(struct connection *connection)
{
    struct request_buffer *buffer = &connection->buffer;
    while (connection->frame == HTTP_INCOMPLETE ||
           buffer->used < (size_t)connection->frame) {
        size_t want = connection->frame == HTTP_INCOMPLETE
                          ? HEADER_MAX
                          : (size_t)connection->frame;
        if (!buffer_reserve(buffer, want)) {
            return request_fail(connection, 500);
        }
        enum tls_status status = buffer_read_some(connection);
        if (status == TLS_CLOSED) {
            return STEP_CLOSE;
        }
        if (status != TLS_DONE) {
            return connection_wait_tls(connection, status);
        }
        connection->frame = http_frame_measure(buffer->data, buffer->used);
        if (connection->frame < HTTP_INCOMPLETE) {
            return request_fail(connection, (int)-connection->frame);
        }
    }
    int status = http_parse_request(buffer->data, (size_t)connection->frame,
                                    &connection->request);
    if (status != 0) {
        return request_fail(connection, status);
    }

    return request_verdict_apply(connection);
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

static enum tls_status buffer_read_some(struct connection *connection)
{
    struct request_buffer *buffer = &connection->buffer;
    size_t count = 0;
    enum tls_status status =
        tls_session_read(connection->session, buffer->data + buffer->used,
                         buffer->capacity - buffer->used, &count);
    if (status == TLS_DONE) {
        buffer->used += count;
        connection_set_deadline(connection, CLIENT_TIMEOUT);
    }

    return status;
}

/* An error reply of the Shared core. The client can still send request
   bytes, so the close drains them first. */
static enum step request_fail(struct connection *connection, int status)
{
    status_reply_plan(connection, status, NULL);
    connection->exchange.needs_drain = true;

    return reply_start(connection);
}

/* A short text reply of rproxy itself. header_lines is NULL, or more
   header lines, each with its CRLF. */
static void status_reply_plan(struct connection *connection, int status,
                              const char *header_lines)
{
    char body[REPLY_TEXT_MAX];
    const char *reason = status_get_reason(status);
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    int len = snprintf(connection->status_text, sizeof(connection->status_text),
                       "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                       "Content-Length: %d\r\n%sConnection: close\r\n\r\n%s",
                       status, reason, body_len,
                       header_lines != NULL ? header_lines : "", body);
    connection->parts[0] = (struct reply_part){.bytes = connection->status_text,
                                               .len = (size_t)len};
    connection->part_count = 1;
    connection->exchange.status = status;
    connection->exchange.reply_bytes = (size_t)len;
}

static const char *status_get_reason(int status)
{
    switch (status) {
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 402:
        return "Payment Required";
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
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

static enum step reply_start(struct connection *connection)
{
    connection->state = STATE_REPLY;
    connection_set_deadline(connection, CLIENT_TIMEOUT);

    return STEP_NEXT;
}

static enum step request_verdict_apply(struct connection *connection)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    struct exchange *exchange = &connection->exchange;
    struct verdict verdict =
        request_decide(rproxy, &connection->request, exchange);
    connection->is_decided = true;
    if (verdict.blocks_client) {
        client_block(rproxy, exchange);
    }
    switch (verdict.kind) {
    case VERDICT_PASS:
        exchange->verdict_name = "pass";
        return request_forward(connection);
    case VERDICT_DENY:
        exchange->verdict_name = "deny";
        status_reply_plan(connection, 403, NULL);
        break;
    case VERDICT_SLOW: {
        char retry_line[HEADER_LINES_MAX];
        snprintf(retry_line, sizeof(retry_line), "Retry-After: %d\r\n",
                 verdict.retry_after);
        exchange->verdict_name = "slow";
        status_reply_plan(connection, 429, retry_line);
        break;
    }
    case VERDICT_TARPIT:
        return tarpit_start(connection, "tarpit");
    case VERDICT_CHALLENGE:
        exchange->verdict_name = "challenge";
        challenge_plan(connection);
        break;
    case VERDICT_PAY:
        exchange->verdict_name = "pay";
        status_reply_plan(connection, 402, "crawler-price: " PAY_PRICE "\r\n");
        break;
    }

    return reply_start(connection);
}

/* The behavior counters count each request of a client IP, this one
   too, before the Verdict. exchange keeps them for the log. The pow and
   the signature are checked only when a flag turns them on. */
static struct verdict request_decide(struct rproxy *rproxy,
                                     const struct request *request,
                                     struct exchange *exchange)
{
    double now = clock_get_seconds();
    struct verdict_signals signals = {.ja4_label = exchange->ja4_label,
                                      .user_agent = request->user_agent,
                                      .target = request->target};
    if (rproxy->limit != NULL) {
        const char *key = rproxy->config.rate_key == RATE_KEY_JA4
                              ? exchange->ja4
                              : exchange->client_ip;
        signals.retry_after = rate_limit_take(rproxy->limit, key, now);
        signals.limit_is_per_ip = rproxy->config.rate_key == RATE_KEY_IP;
    }
    exchange->has_behavior =
        behavior_add_request(rproxy->counters, exchange->client_ip,
                             request->target, now, &exchange->behavior);
    signals.behavior = exchange->behavior;
    const struct rproxy_config *config = &rproxy->config;
    signals.is_pow_target = target_has_prefix(
        request->target, config->pow_prefixes, config->pow_prefix_count);
    signals.pow_ok =
        signals.is_pow_target && request_has_pow(rproxy, request, exchange);
    signals.is_pay_target = target_has_prefix(
        request->target, config->pay_prefixes, config->pay_prefix_count);
    signals.bot_verified = request_is_signed(rproxy, request);

    return verdict_decide(&signals);
}

/* A plain prefix of the target, as an nginx "location /data/" prefix. */
static bool target_has_prefix(const char *target, const char *const *prefixes,
                              int count)
{
    for (int i = 0; i < count; i++) {
        if (strncmp(target, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }

    return false;
}

static bool request_has_pow(const struct rproxy *rproxy,
                            const struct request *request,
                            const struct exchange *exchange)
{
    return pow_is_solved(http_find_header(request, "X-PoW"),
                         exchange->client_ip, wall_clock_get_seconds(),
                         rproxy->pow_secret, POW_SECRET_LEN,
                         rproxy->config.pow_bits);
}

/* The pow nonce and the signature times are wall clock times, so that
   they mean the same thing to the client. */
static double wall_clock_get_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (double)now.tv_sec + (double)now.tv_nsec / NANOSECONDS_PER_SECOND;
}

/* The signature covers @authority, so a request with no Host cannot
   be verified. */
static bool request_is_signed(const struct rproxy *rproxy,
                              const struct request *request)
{
    if (rproxy->keys == NULL || request->host == NULL) {
        return false;
    }
    struct bot_auth_message message = {.method = request->method,
                                       .authority = request->host,
                                       .target = request->target,
                                       .find_header = request_find_header,
                                       .context = request};

    return bot_auth_decide(&message, rproxy->keys, wall_clock_get_seconds(),
                           NULL) == BOT_AUTH_VERIFIED;
}

static const char *request_find_header(const void *context, const char *name)
{
    return http_find_header(context, name);
}

/* The next connection of the client IP drops at the connection Verdict,
   until --block-time ends. The block comes before a tarpit, so that the
   other connections of the client drop while this one is held. */
static void client_block(struct rproxy *rproxy, const struct exchange *exchange)
{
    double now = clock_get_seconds();
    block_table_add(rproxy->blocks, exchange->client_ip,
                    now + rproxy->config.block_time, now);
}

/* A 401 with the challenge for the client IP in the current window. RFC
   9110 section 15.5.2: a 401 has WWW-Authenticate. The scheme follows
   PrivateToken of RFC 9577: the challenge is an auth-param. */
static void challenge_plan(struct connection *connection)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    char challenge[POW_CHALLENGE_MAX + 1];
    char lines[HEADER_LINES_MAX];
    pow_format_challenge(connection->exchange.client_ip,
                         wall_clock_get_seconds(), rproxy->pow_secret,
                         POW_SECRET_LEN, rproxy->config.pow_bits, challenge);
    snprintf(lines, sizeof(lines),
             "X-PoW-Challenge: %s\r\n"
             "WWW-Authenticate: PoW challenge=\"%s\"\r\n",
             challenge, challenge);
    status_reply_plan(connection, 401, lines);
}

/* Only a PASS reaches here, so a denied request never reaches the cache.
   A hit does not reach an Origin. */
static enum step request_forward(struct connection *connection)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    char key[CACHE_KEY_MAX];
    if (rproxy->cache != NULL &&
        cache_key_format(&connection->request, key, sizeof(key)) &&
        cache_find(rproxy->cache, key, time(NULL), &connection->reply,
                   &connection->reply_len)) {
        connection->exchange.cache_name = "hit";
        reply_plan_origin(connection);
        return reply_start(connection);
    }

    return origin_exchange_start(connection);
}

/* The key is the method, the path, and the query.
   Returns false if the key does not fit. */
static bool cache_key_format(const struct request *request, char *key,
                             size_t size)
{
    int key_len =
        snprintf(key, size, "%s %s", request->method, request->target);

    return key_len >= 0 && (size_t)key_len < size;
}

/* A reply from the cache or from an Origin. With the cache on, a matching
   If-None-Match gets a 304, and a full reply gets X-Cache. */
static void reply_plan_origin(struct connection *connection)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    struct exchange *exchange = &connection->exchange;
    bool is_planned =
        rproxy->cache != NULL &&
        reply_is_not_modified(&connection->request, connection->reply,
                              connection->reply_len) &&
        not_modified_plan(connection);
    if (!is_planned) {
        reply_plan_parts(connection,
                         rproxy->cache != NULL ? exchange->cache_name : NULL);
        exchange->status =
            reply_get_status(connection->reply, connection->reply_len);
        exchange->reply_bytes = connection->reply_len;
    }
}

/* A GET whose If-None-Match names the ETag of a 200 reply (RFC 9110
   section 13.1.2). The Origin never sees If-None-Match (forward.c). */
static bool reply_is_not_modified(const struct request *request,
                                  const char *reply, size_t reply_len)
{
    const char *if_none_match = http_find_header(request, "If-None-Match");

    return if_none_match != NULL && strcmp(request->method, "GET") == 0 &&
           reply_get_status(reply, reply_len) == 200 &&
           cache_is_not_modified(reply, reply_len, if_none_match);
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

/* A 304 with the ETag and X-Cache, and no body. Returns false, and plans
   nothing, if the ETag does not fit. On a miss the log keeps the reply
   bytes from the Origin, for the Scene; on a hit it gets the 304 bytes. */
static bool not_modified_plan(struct connection *connection)
{
    struct exchange *exchange = &connection->exchange;
    size_t etag_len = 0;
    const char *etag =
        cache_find_etag(connection->reply, connection->reply_len, &etag_len);
    bool is_hit = strcmp(exchange->cache_name, "hit") == 0;
    int len = snprintf(connection->status_text, sizeof(connection->status_text),
                       "HTTP/1.1 304 Not Modified\r\nX-Cache: %s\r\n"
                       "ETag: %.*s\r\nConnection: close\r\n\r\n",
                       is_hit ? "HIT" : "MISS", (int)etag_len, etag);
    if (len < 0 || (size_t)len >= sizeof(connection->status_text)) {
        return false;
    }
    connection->parts[0] = (struct reply_part){.bytes = connection->status_text,
                                               .len = (size_t)len};
    connection->part_count = 1;
    exchange->status = 304;
    exchange->reply_bytes = is_hit ? (size_t)len : connection->reply_len;

    return true;
}

/* cache_name NULL sends the reply as it is. Otherwise "X-Cache: HIT" or
   "MISS" goes after the status line. The parts point into the reply, so
   no byte is copied. */
static void reply_plan_parts(struct connection *connection,
                             const char *cache_name)
{
    const char *reply = connection->reply;
    size_t reply_len = connection->reply_len;
    const char *line_end =
        cache_name == NULL ? NULL : memchr(reply, '\n', reply_len);
    if (line_end == NULL) {
        connection->parts[0] = (struct reply_part){reply, reply_len};
        connection->part_count = 1;
        return;
    }
    size_t status_len = (size_t)(line_end - reply) + 1;
    const char *header = strcmp(cache_name, "hit") == 0 ? "X-Cache: HIT\r\n"
                                                        : "X-Cache: MISS\r\n";
    connection->parts[0] = (struct reply_part){reply, status_len};
    connection->parts[1] = (struct reply_part){header, strlen(header)};
    connection->parts[2] =
        (struct reply_part){reply + status_len, reply_len - status_len};
    connection->part_count = REPLY_PARTS_MAX;
}

/* The whole exchange with the Origin, the connects included, must end in
   --origin-timeout seconds. In backpressure only the connect and the send
   must: then the deadline moves at each Origin read. */
static enum step origin_exchange_start(struct connection *connection)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    int status = exchange_slot_take(connection);
    if (status == 0 && !origin_request_format(connection)) {
        status = 500;
    }
    if (status != 0) {
        status_reply_plan(connection, status, NULL);
        return reply_start(connection);
    }
    connection->origin_index = origin_list_pick(&rproxy->list);
    connection->exchange.origin_index = (int)connection->origin_index;
    connection_set_deadline(connection, rproxy->config.origin_timeout);

    return origin_connect_start(connection);
}

/* RELAY_CONN_CAP (SPEC 12, Act 3): one slot of --relay-conns for each
   connection that reaches an Origin. It stays taken until the connection
   closes, so a slow client holds it for its whole download. Returns 0, or
   503 if no slot is free. */
static int exchange_slot_take(struct connection *connection)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    if (rproxy->config.relay_mode != RELAY_CONN_CAP) {
        return 0;
    }
    int in_progress = atomic_fetch_add(&rproxy->origin_exchanges, 1);
    if (in_progress >= rproxy->config.relay_conns) {
        atomic_fetch_sub(&rproxy->origin_exchanges, 1);
        return 503;
    }
    connection->holds_exchange_slot = true;
    client_send_buffer_cap(connection);

    return 0;
}

/* Autotuning grows the kernel send buffer of conn_fd up to tcp_wmem
   (4 MiB), and that memory is charged to rproxy. In conn-cap a 4 MiB
   buffer also let rproxy close (and free the slot) about 10 s before a
   400 KB/s client had its last byte: 29 Phones finished, not 20. So both
   modes cap it at --relay-buffer. */
static void client_send_buffer_cap(struct connection *connection)
{
    int relay_buffer = connection->loop->rproxy->config.relay_buffer;
    setsockopt(connection->conn_fd, SOL_SOCKET, SO_SNDBUF, &relay_buffer,
               sizeof(relay_buffer));
}

static bool origin_request_format(struct connection *connection)
{
    const struct exchange *exchange = &connection->exchange;
    size_t size = HEADER_MAX + FORWARD_EXTRA_MAX + connection->request.body_len;
    connection->origin_request = malloc(size);
    if (connection->origin_request == NULL) {
        return false;
    }
    connection->origin_request_len =
        forward_format_request(&connection->request, exchange->client_ip,
                               exchange->ja4, connection->origin_request, size);

    return connection->origin_request_len > 0;
}

/* The connect ends when the Origin socket is writable (connect(2),
   EINPROGRESS). */
static enum step origin_connect_start(struct connection *connection)
{
    const struct origin_list *list = &connection->loop->rproxy->list;
    connection->origin_fd = origin_connect(
        &list->origins[connection->origin_index], connection->address_index,
        origin_get_receive_buffer(connection->loop->rproxy));
    if (connection->origin_fd < 0) {
        return origin_connect_retry(connection);
    }
    struct epoll_event event = {.events = EPOLLOUT | EPOLLET,
                                .data.ptr = &connection->origin_end};
    if (epoll_ctl(connection->loop->epoll_fd, EPOLL_CTL_ADD,
                  connection->origin_fd, &event) != 0) {
        return origin_connect_retry(connection);
    }
    connection->origin_events = EPOLLOUT;
    connection->state = STATE_ORIGIN_CONNECT;

    return STEP_WAIT;
}

/* Backpressure: while rproxy does not read, the Origin bytes wait in
   the receive queue of origin_fd, which is memory of rproxy too. Without
   a cap, 200 slow clients took 126 MiB of socket memory (2026-11-25). */
static int origin_get_receive_buffer(const struct rproxy *rproxy)
{
    return rproxy->config.relay_mode == RELAY_BACKPRESSURE
               ? rproxy->config.relay_buffer
               : 0;
}

/* A failed connect tries the next address of the same Origin, as
   forward_fetch tried each getaddrinfo result. When the addresses end, it
   tries the next Origin once, as nginx proxy_next_upstream error. Then the
   client gets 502. */
static enum step origin_connect_retry(struct connection *connection)
{
    const struct origin_list *list = &connection->loop->rproxy->list;
    const struct origin *origin = &list->origins[connection->origin_index];
    origin_socket_close(connection);
    if (connection->address_index + 1 < origin->address_count) {
        connection->address_index++;
        return origin_connect_start(connection);
    }
    if (connection->has_tried_next || list->count < 2) {
        return origin_exchange_end(connection, 502);
    }
    connection->has_tried_next = true;
    connection->origin_index = (connection->origin_index + 1) % list->count;
    connection->address_index = 0;

    return origin_connect_start(connection);
}

/* close() also takes the socket out of the epoll instance. */
static void origin_socket_close(struct connection *connection)
{
    if (connection->origin_fd >= 0) {
        close(connection->origin_fd);
        connection->origin_fd = -1;
    }
}

/* status 0: the reply from the Origin goes into the cache and to the
   client. Any other status: the client gets that status. */
static enum step origin_exchange_end(struct connection *connection, int status)
{
    struct rproxy *rproxy = connection->loop->rproxy;
    origin_socket_close(connection);
    free(connection->origin_request);
    connection->origin_request = NULL;
    if (status != 0) {
        status_reply_plan(connection, status, NULL);
        return reply_start(connection);
    }
    char key[CACHE_KEY_MAX];
    if (rproxy->cache != NULL &&
        cache_key_format(&connection->request, key, sizeof(key))) {
        cache_store(rproxy->cache, key, connection->reply,
                    connection->reply_len, time(NULL));
    }
    connection->exchange.cache_name = rproxy->cache != NULL ? "miss" : "-";
    reply_plan_origin(connection);

    return reply_start(connection);
}

static enum step origin_connect_step(struct connection *connection)
{
    if (!origin_is_connected(connection->origin_fd)) {
        return origin_connect_retry(connection);
    }
    connection->state = STATE_ORIGIN_SEND;

    return STEP_NEXT;
}

static enum step origin_send_step(struct connection *connection)
{
    while (connection->origin_request_sent < connection->origin_request_len) {
        ssize_t written = write(
            connection->origin_fd,
            connection->origin_request + connection->origin_request_sent,
            connection->origin_request_len - connection->origin_request_sent);
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return STEP_WAIT;
        }
        if (written < 0) {
            return origin_exchange_end(connection, 502);
        }
        connection->origin_request_sent += (size_t)written;
    }
    connection_watch_origin(connection, EPOLLIN);
    if (connection->loop->rproxy->config.relay_mode == RELAY_BACKPRESSURE) {
        return relay_start(connection);
    }
    connection->state = STATE_ORIGIN_READ;

    return STEP_NEXT;
}

/* Edge-triggered, as the client socket. events 0 stops the wake-ups for
   reads; a MOD back to EPOLLIN reports bytes that wait already. */
static void connection_watch_origin(struct connection *connection,
                                    uint32_t events)
{
    if (connection->origin_fd < 0 || connection->origin_events == events) {
        return;
    }
    struct epoll_event event = {.events = events | EPOLLET,
                                .data.ptr = &connection->origin_end};
    epoll_ctl(connection->loop->epoll_fd, EPOLL_CTL_MOD, connection->origin_fd,
              &event);
    connection->origin_events = events;
}

/* SPEC 12, Act 4: the Origin bytes go through a buffer of --relay-buffer
   bytes. The reply goes to the client as it is: no X-Cache, no 304. */
static enum step relay_start(struct connection *connection)
{
    struct relay_buffer *relay = &connection->relay;
    relay->capacity = (size_t)connection->loop->rproxy->config.relay_buffer;
    relay->data = malloc(relay->capacity);
    if (relay->data == NULL) {
        return origin_exchange_end(connection, 502);
    }
    /* 200 slow clients took 392 MiB of socket memory without the cap
       (2026-11-25). */
    client_send_buffer_cap(connection);
    connection->state = STATE_RELAY;

    return STEP_NEXT;
}

/* Turns of "read the Origin into the room" and "send the bytes to the
   client", until neither moves a byte. Then it waits for the side that
   holds the relay back. */
static enum step relay_step(struct connection *connection)
{
    struct relay_buffer *relay = &connection->relay;
    bool has_moved = false;
    for (;;) {
        bool is_turn_moved = false;
        if (!relay_read_origin(connection, &is_turn_moved)) {
            return origin_exchange_end(connection, 502);
        }
        enum tls_status status = relay_send_client(connection, &is_turn_moved);
        if (status == TLS_CLOSED) {
            return STEP_CLOSE;
        }
        if (relay->origin_is_done && relay->sent == relay->used) {
            return reply_end(connection);
        }
        if (!is_turn_moved) {
            return relay_wait(connection, status, has_moved);
        }
        has_moved = true;
    }
}

/* Reads until the room is full, the Origin has no bytes now, or EOF.
   Returns false for an Origin that fails before its first byte: the
   client can still get a 502. A later failure ends the reply early. */
static bool relay_read_origin(struct connection *connection, bool *has_moved)
{
    struct relay_buffer *relay = &connection->relay;
    struct exchange *exchange = &connection->exchange;
    relay_compact(relay);
    while (!relay->origin_is_done && relay->used < relay->capacity) {
        ssize_t length = read(connection->origin_fd, relay->data + relay->used,
                              relay->capacity - relay->used);
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (length <= 0) {
            if (exchange->reply_bytes == 0) {
                return false;
            }
            relay_end_origin(connection);
            break;
        }
        relay->used += (size_t)length;
        exchange->reply_bytes += (size_t)length;
        *has_moved = true;
        /* The status line is at the start while no byte moved. */
        if (exchange->status == 0 && exchange->reply_bytes == relay->used) {
            exchange->status = reply_get_status(relay->data, relay->used);
        }
    }

    return true;
}

/* Moves the bytes that wait for the client to the start, so that all the
   rest is room. The TLS session allows a moved buffer
   (SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER in tls.c). */
static void relay_compact(struct relay_buffer *relay)
{
    if (relay->sent == 0) {
        return;
    }
    memmove(relay->data, relay->data + relay->sent, relay->used - relay->sent);
    relay->used -= relay->sent;
    relay->sent = 0;
}

static void relay_end_origin(struct connection *connection)
{
    connection->relay.origin_is_done = true;
    origin_socket_close(connection);
    free(connection->origin_request);
    connection->origin_request = NULL;
}

/* Sends until the buffer is empty or the TLS session wants the socket.
   After a want, the same bytes go again (SSL_write(3)). */
static enum tls_status relay_send_client(struct connection *connection,
                                         bool *has_moved)
{
    struct relay_buffer *relay = &connection->relay;
    while (relay->sent < relay->used) {
        size_t count = 0;
        enum tls_status status =
            tls_session_send(connection->session, relay->data + relay->sent,
                             relay->used - relay->sent, &count);
        if (status != TLS_DONE) {
            return status;
        }
        relay->sent += count;
        *has_moved = true;
    }

    return TLS_DONE;
}

/* Back pressure: with no room, the Origin socket is not watched, so the
   kernel buffers of the Origin fill and TCP slows the Origin. The deadline
   moves only when bytes moved. It belongs to the side that the relay
   waits for: the client while bytes wait for it, else the Origin. */
static enum step relay_wait(struct connection *connection,
                            enum tls_status status, bool has_moved)
{
    struct relay_buffer *relay = &connection->relay;
    bool has_room = relay->used < relay->capacity;
    connection_watch_origin(connection, has_room ? EPOLLIN : 0);
    if (has_moved) {
        bool waits_for_client = relay->sent < relay->used;
        connection_set_deadline(
            connection, waits_for_client
                            ? CLIENT_TIMEOUT
                            : connection->loop->rproxy->config.origin_timeout);
    }
    if (status != TLS_DONE) {
        return connection_wait_tls(connection, status);
    }
    connection_watch_client(connection, EPOLLIN);

    return STEP_WAIT;
}

/* Reads to EOF (the request has Connection: close). An empty reply, or
   with reply-cap a reply over ORIGIN_REPLY_MAX, is 502. */
static enum step origin_read_step(struct connection *connection)
{
    for (;;) {
        if (!reply_reserve(connection)) {
            return origin_exchange_end(connection, 502);
        }
        ssize_t length = read(
            connection->origin_fd, connection->reply + connection->reply_len,
            connection->reply_capacity - connection->reply_len);
        if (length == 0) {
            return origin_exchange_end(connection,
                                       connection->reply_len > 0 ? 0 : 502);
        }
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return STEP_WAIT;
        }
        if (length < 0) {
            return origin_exchange_end(connection, 502);
        }
        connection->reply_len += (size_t)length;
        if (reply_is_over_cap(connection)) {
            return origin_exchange_end(connection, 502);
        }
    }
}

/* Only reply-cap has a size limit. conn-cap limits the count of the
   replies instead (SPEC 12, Act 3: 20 phones get the whole file). */
static bool reply_is_over_cap(const struct connection *connection)
{
    return connection->loop->rproxy->config.relay_mode == RELAY_REPLY_CAP &&
           connection->reply_len > ORIGIN_REPLY_MAX;
}

/* Keeps room for one more ORIGIN_READ_CHUNK. The room doubles, so a
   large reply costs few copies. */
static bool reply_reserve(struct connection *connection)
{
    size_t need = connection->reply_len + ORIGIN_READ_CHUNK;
    if (connection->reply_capacity >= need) {
        return true;
    }
    size_t capacity = connection->reply_capacity * 2;
    if (capacity < need) {
        capacity = need;
    }
    char *reply = realloc(connection->reply, capacity);
    if (reply == NULL) {
        return false;
    }
    connection->reply = reply;
    connection->reply_capacity = capacity;

    return true;
}

/* Sends the parts in order. After a want, the same bytes go again
   (SSL_write(3)). */
static enum step reply_step(struct connection *connection)
{
    while (connection->part_index < connection->part_count) {
        const struct reply_part *part =
            &connection->parts[connection->part_index];
        if (connection->part_offset == part->len) {
            reply_advance(connection, 0);
            continue;
        }
        size_t count = 0;
        enum tls_status status = tls_session_send(
            connection->session, part->bytes + connection->part_offset,
            part->len - connection->part_offset, &count);
        if (status == TLS_CLOSED) {
            return STEP_CLOSE;
        }
        if (status != TLS_DONE) {
            return connection_wait_tls(connection, status);
        }
        connection_set_deadline(connection, CLIENT_TIMEOUT);
        reply_advance(connection, count);
    }

    return reply_end(connection);
}

static void reply_advance(struct connection *connection, size_t count)
{
    connection->part_offset += count;
    if (connection->part_offset ==
        connection->parts[connection->part_index].len) {
        connection->part_index++;
        connection->part_offset = 0;
    }
}

/* After an error reply, unread request bytes make close() send RST, and
   the client can lose the reply. So send close_notify, stop writing, and
   read and drop the rest for a short time, so that close() sends FIN. */
static enum step reply_end(struct connection *connection)
{
    if (!connection->exchange.needs_drain) {
        return STEP_CLOSE;
    }
    tls_session_destroy(connection->session);
    connection->session = NULL;
    shutdown(connection->conn_fd, SHUT_WR);
    connection_watch_client(connection, EPOLLIN);
    connection->state = STATE_DRAIN;
    connection_set_deadline(connection, DRAIN_TIMEOUT);

    return STEP_NEXT;
}

static enum step drain_step(struct connection *connection)
{
    while (connection->drain_reads < DRAIN_READS_MAX) {
        ssize_t count = read(connection->conn_fd, connection->loop->scratch,
                             DRAIN_CHUNK_SIZE);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return STEP_WAIT;
        }
        if (count <= 0) {
            return STEP_CLOSE;
        }
        connection->drain_reads++;
    }

    return STEP_CLOSE;
}

/* Logs the connection and closes its sockets. The memory stays until the
   end of the loop turn, because a later event of the same turn can still
   point to it. */
static void connection_close(struct connection *connection)
{
    struct event_loop *loop = connection->loop;
    if (connection->is_decided) {
        behavior_add_reply(loop->rproxy->counters,
                           connection->exchange.client_ip,
                           connection->exchange.status, clock_get_seconds());
    }
    timer_heap_remove(loop->timers, &connection->timer);
    exchange_slot_release(connection);
    origin_socket_close(connection);
    tls_session_destroy(connection->session);
    connection->session = NULL;
    close(connection->conn_fd);
    exchange_log(&connection->exchange);
    if (connection->prev != NULL) {
        connection->prev->next = connection->next;
    } else {
        loop->connections = connection->next;
    }
    if (connection->next != NULL) {
        connection->next->prev = connection->prev;
    }
    loop->connection_count--;
    connection->state = STATE_CLOSED;
    connection->next = loop->closed;
    loop->closed = connection;
}

static void exchange_slot_release(struct connection *connection)
{
    if (!connection->holds_exchange_slot) {
        return;
    }
    atomic_fetch_sub(&connection->loop->rproxy->origin_exchanges, 1);
    connection->holds_exchange_slot = false;
}

/* One line for each connection: client IP, JA4, Verdict, cache result,
   Origin index, status, reply bytes. "-" marks a value that the connection
   did not reach. On a cache miss or with the cache off, the reply bytes are
   the bytes that came from the Origin. */
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
    fprintf(stderr, "%s %s %s %s %s %s %zu\n", exchange->client_ip,
            exchange->ja4[0] != '\0' ? exchange->ja4 : "-",
            exchange->verdict_name, exchange->cache_name, origin_text,
            status_text, exchange->reply_bytes);
    behavior_log(exchange);
}

/* A second line after a request Verdict: the behavior counters of the
   client IP with this request, before its reply. It starts with
   "behavior", so a reader of the 7-field line can skip it. */
static void behavior_log(const struct exchange *exchange)
{
    if (!exchange->has_behavior) {
        return;
    }
    const struct behavior_counts *counts = &exchange->behavior;
    fprintf(stderr,
            "behavior %s requests=%u targets=%u not_found=%u robots=%u "
            "trap=%u slowed=%u\n",
            exchange->client_ip, counts->requests, counts->targets,
            counts->not_found, counts->robots_reads, counts->trap_hits,
            counts->slowed);
}

static void loop_expire_timers(struct event_loop *loop)
{
    double now = clock_get_seconds();
    struct timer *timer = NULL;
    while ((timer = timer_heap_remove_expired(loop->timers, now)) != NULL) {
        struct connection *connection =
            (struct connection *)(void *)((char *)timer -
                                          offsetof(struct connection, timer));
        connection_expire(connection);
    }
}

/* What a deadline means in each state. In the connect the Origin gave no
   connection (502); later it gave no reply in time (504). */
static void connection_expire(struct connection *connection)
{
    switch (connection->state) {
    case STATE_HELLO:
        connection->exchange.verdict_name = "no-tls";
        break;
    case STATE_HANDSHAKE:
        connection->exchange.verdict_name = "no-handshake";
        break;
    case STATE_ORIGIN_CONNECT:
        origin_exchange_end(connection, 502);
        connection_advance(connection);
        return;
    case STATE_ORIGIN_SEND:
    case STATE_ORIGIN_READ:
        origin_exchange_end(connection, 504);
        connection_advance(connection);
        return;
    case STATE_RELAY:
        relay_expire(connection);
        return;
    default:
        break;
    }
    connection_close(connection);
}

/* Before the first Origin byte the client can still get a 504. After it,
   the reply has started, so the connection only closes. */
static void relay_expire(struct connection *connection)
{
    if (connection->exchange.reply_bytes == 0) {
        origin_exchange_end(connection, 504);
        connection_advance(connection);
        return;
    }
    connection_close(connection);
}

static void loop_free_closed(struct event_loop *loop)
{
    while (loop->closed != NULL) {
        struct connection *connection = loop->closed;
        loop->closed = connection->next;
        free(connection->buffer.data);
        free(connection->origin_request);
        free(connection->reply);
        free(connection->relay.data);
        free(connection);
    }
}

/* At a stop: rproxy does not wait for the open requests. A connection
   with no Verdict yet logs the Verdict stop. */
static void loop_close_all(struct event_loop *loop)
{
    while (loop->connections != NULL) {
        struct exchange *exchange = &loop->connections->exchange;
        if (strcmp(exchange->verdict_name, "-") == 0) {
            exchange->verdict_name = "stop";
        }
        connection_close(loop->connections);
    }
    loop_free_closed(loop);
}
