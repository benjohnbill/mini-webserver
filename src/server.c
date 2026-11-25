/* Accept loop, worker thread pool and request dispatch.
 *
 * Concurrency model: one acceptor thread (main) pushes connected sockets
 * into a bounded queue; N worker threads pop and serve them, one
 * connection at a time, with HTTP keep-alive. When the queue is full the
 * acceptor answers 503 immediately instead of letting clients hang. */
#ifdef __linux__
#define _GNU_SOURCE                   /* accept4() */
#endif
#include "server_run.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

#include "cgi.h"
#include "health.h"
#include "http.h"
#include "proxy.h"
#include "route.h"

#define SEND_TIMEOUT_MS 10000         /* a stuck reader must not pin a worker */

static volatile sig_atomic_t g_stop = 0;
static int g_busy;                    /* workers inside handle_connection, __atomic */
static unsigned long g_shed_queue;    /* 503: queue full at accept, __atomic */
static unsigned long g_shed_stale;    /* 503: waited > -W, __atomic */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* bounded connection queue                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    char ip[INET6_ADDRSTRLEN];
    int64_t accepted_ms;              /* CLOCK_MONOTONIC */
} client;

typedef struct {
    client *items;
    size_t cap, head, count;
    int closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
} client_queue;

static int queue_init(client_queue *q, size_t cap)
{
    q->items = calloc(cap, sizeof *q->items);
    if (q->items == NULL)
        return -1;
    q->cap = cap;
    q->head = q->count = 0;
    q->closed = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

/* Non-blocking push: returns -1 when full so the caller can shed load. */
static int queue_push(client_queue *q, const client *c)
{
    int ok = -1;
    pthread_mutex_lock(&q->mu);
    if (!q->closed && q->count < q->cap) {
        q->items[(q->head + q->count) % q->cap] = *c;
        q->count++;
        ok = 0;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

/* Blocks until a client is available; returns -1 once the queue is closed. */
static int queue_pop(client_queue *q, client *out)
{
    int ok = -1;
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mu);
    if (!q->closed) {
        *out = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        ok = 0;
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

/* Are connections waiting for a worker? */
static int queue_has_waiting(client_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int waiting = q->count > 0;
    pthread_mutex_unlock(&q->mu);
    return waiting;
}

static size_t queue_count(client_queue *q)
{
    pthread_mutex_lock(&q->mu);
    size_t n = q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}

static void queue_close(client_queue *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

/* Only after all workers are joined: closes connections never served. */
static void queue_destroy(client_queue *q)
{
    for (size_t i = 0; i < q->count; i++)
        close(q->items[(q->head + i) % q->cap].fd);
    free(q->items);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
}

/* ------------------------------------------------------------------ */
/* response helpers                                                    */
/* ------------------------------------------------------------------ */

/* Context */
typedef struct {
    server_config *cfg;
    int fd;
    const char *ip;
    http_request *req;
    int keep_alive;                   /* cleared when the conn must close */
    client_queue *queue;              /* for /_lb/status */
} ctx;

static int is_head(const ctx *x) { return strcmp(x->req->method, "HEAD") == 0; }

/* extra headers = given lines + "X-Server-Name" when configured */
static int with_name(const ctx *x, const char *extra, char *out, size_t size)
{
    int w;
    if (x->cfg->name[0])
        w = snprintf(out, size, "%sX-Server-Name: %s\r\n", extra ? extra : "",
                     x->cfg->name);
    else
        w = snprintf(out, size, "%s", extra ? extra : "");
    return (w < 0 || (size_t)w >= size) ? -1 : 0;
}

static int reply(ctx *x, int status, const char *type, const char *body,
                 size_t len, const char *extra)
{
    char hdrs[3072];
    if (with_name(x, extra, hdrs, sizeof hdrs) < 0)
        hdrs[0] = '\0';
    if (http_send_response(x->fd, status, type, body, len, x->keep_alive,
                           hdrs, is_head(x)) < 0)
        x->keep_alive = 0;
    return status;
}

/* Error page that respects HEAD (no body) and can carry extra headers
 * such as "Allow:" for 405. */
static int reply_error(ctx *x, int status, const char *extra)
{
    char page[512];
    const char *t = http_status_text(status);
    int n = snprintf(page, sizeof page,
                     "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>mini-webserver</p></body></html>\n",
                     status, t, status, t);
    if (n < 0 || (size_t)n >= sizeof page)
        n = 0;
    if (status == 503 && extra == NULL)
        extra = "Retry-After: 1\r\n";   /* same as http_send_error */
    return reply(x, status, "text/html; charset=utf-8", page, (size_t)n, extra);
}

static int errno_status(int err)
{
    if (err == ENOENT || err == ENOTDIR || err == ENAMETOOLONG)
        return 404;
    if (err == EACCES || err == ELOOP)
        return 403;
    return 500;
}

/* resolved must equal root or lie below it ("/www2" is not inside "/www"). */
static int inside(const char *resolved, const char *root)
{
    size_t rl = strlen(root);
    if (rl == 1 && root[0] == '/')
        return 1;
    return strncmp(resolved, root, rl) == 0 &&
           (resolved[rl] == '\0' || resolved[rl] == '/');
}

/* ------------------------------------------------------------------ */
/* static files                                                        */
/* ------------------------------------------------------------------ */

static int stream_file(int sock, int file, off_t size)
{
#ifdef __linux__
    /* Zero-copy kernel path. */
    off_t off = 0;
    while (off < size) {
        ssize_t n = sendfile(sock, file, &off, (size_t)(size - off));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;                  /* file shrank under us */
    }
    return 0;
#else
    char buf[65536];
    off_t left = size;
    while (left > 0) {
        size_t want = left < (off_t)sizeof buf ? (size_t)left : sizeof buf;
        ssize_t n = read(file, buf, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        if (write_all(sock, buf, (size_t)n) < 0)
            return -1;
        left -= n;
    }
    return 0;
#endif
}

static int serve_static(ctx *x)
{
    const http_request *req = x->req;
    const char *root = x->cfg->root;

    /* 파일 조회는 GET, HEAD 메소드만 혀용. */
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0)
        return reply_error(x, 405, "Allow: GET, HEAD\r\n");

    /* 요청 경로를 실제 파일 경로로 변환. 
     * 요청 경로와 서버의 root 경로를 합친 경로가
     * 너무 길 경우 404 반환
     */
    char candidate[PATH_MAX], real[PATH_MAX];
    int w = snprintf(candidate, sizeof candidate, "%s%s", root, req->path);
    if (w < 0 || (size_t)w >= sizeof candidate)
        return reply_error(x, 404, NULL);

    /* realpath() collapses "..", "." and symlinks; the prefix check then
     * decides whether the target is really inside the document root. */
    if (realpath(candidate, real) == NULL)
        return reply_error(x, errno_status(errno), NULL);

    /* Prevents Directory / Path travelsal attack */
    if (!inside(real, root))
        return reply_error(x, 403, NULL);

    struct stat st;
    if (stat(real, &st) != 0)
        return reply_error(x, errno_status(errno), NULL);

    if (S_ISDIR(st.st_mode)) {
        size_t pl = strlen(req->path);
        if (pl == 0 || req->path[pl - 1] != '/') {
            /* "/docs" -> "/docs/" so relative links in index.html work. */
            char loc[HTTP_MAX_URI + 32];
            size_t raw = strcspn(req->uri, "?#");
            if (req->uri[0] != '/')
                return reply_error(x, 404, NULL);
            w = snprintf(loc, sizeof loc, "Location: %.*s/%s%s\r\n", (int)raw,
                         req->uri, req->query[0] ? "?" : "", req->query);
            if (w < 0 || (size_t)w >= sizeof loc)
                return reply_error(x, 414, NULL);
            return reply_error(x, 301, loc);
        }
        char index[PATH_MAX];
        w = snprintf(index, sizeof index, "%s/index.html", real);
        if (w < 0 || (size_t)w >= sizeof index)
            return reply_error(x, 404, NULL);
        if (realpath(index, real) == NULL)
            return reply_error(x, errno_status(errno), NULL);
        if (!inside(real, root))
            return reply_error(x, 403, NULL);
        if (stat(real, &st) != 0)
            return reply_error(x, errno_status(errno), NULL);
    }
    if (!S_ISREG(st.st_mode))
        return reply_error(x, 403, NULL); /* devices, fifos, sockets */

    int file = open(real, O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return reply_error(x, errno_status(errno), NULL);
    if (fstat(file, &st) != 0) {
        close(file);
        return reply_error(x, 500, NULL);
    }

    /* Headers first (Content-Length = file size), then stream the file. */
    char hdrs[256];
    if (with_name(x, NULL, hdrs, sizeof hdrs) < 0)
        hdrs[0] = '\0';
    if (http_send_response(x->fd, 200, mime_type_for_path(real), NULL,
                           (size_t)st.st_size, x->keep_alive, hdrs, 1) < 0 ||
        (!is_head(x) && stream_file(x->fd, file, st.st_size) < 0))
        x->keep_alive = 0;              /* client gone or truncated body */
    close(file);
    return 200;
}

/* ------------------------------------------------------------------ */
/* /echo and CGI                                                       */
/* ------------------------------------------------------------------ */

/* Built-in dynamic endpoint: reflects the request. Handy as a backend
 * for reverse-proxy demos (-n shows which instance answered). */
static int serve_echo(ctx *x)
{
    const http_request *req = x->req;
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0 &&
        strcmp(req->method, "HEAD") != 0)
        return reply_error(x, 405, "Allow: GET, POST, HEAD\r\n");

    size_t cap = 256 + strlen(req->method) + strlen(req->path) +
                 strlen(req->query) + strlen(x->cfg->name) + req->body_len;
    char *out = malloc(cap);
    if (out == NULL)
        return reply_error(x, 500, NULL);
    int n = snprintf(out, cap,
                     "method: %s\npath: %s\nquery: %s\nserver: %s\n"
                     "body-length: %zu\n\n",
                     req->method, req->path, req->query,
                     x->cfg->name[0] ? x->cfg->name : "-", req->body_len);
    if (n < 0 || (size_t)n + req->body_len > cap) {
        free(out);
        return reply_error(x, 500, NULL);
    }
    if (req->body_len > 0)
        memcpy(out + n, req->body, req->body_len); /* binary-safe */
    reply(x, 200, "text/plain; charset=utf-8", out, (size_t)n + req->body_len,
          NULL);
    free(out);
    return 200;
}

static int serve_cgi(ctx *x)
{
    cgi_result res;
    int st = cgi_run(x->req, x->cfg, x->ip, &res);
    if (st != 0)
        return reply_error(x, st, NULL);
    reply(x, res.status, res.content_type, res.body, res.body_len, res.headers);
    st = res.status;
    cgi_result_free(&res);
    return st;
}

/* ------------------------------------------------------------------ */
/* dispatch + connection loop                                          */
/* ------------------------------------------------------------------ */

int path_has_dot_segment(const char *path)
{
    for (const char *s = path; *s; ) {
        while (*s == '/')
            s++;
        size_t n = strcspn(s, "/");
        if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 1;
        s += n;
    }
    return 0;
}

/* Loopback peers only: the table exposes backend topology. */
static int serve_lb_status(ctx *x)
{
    if (strncmp(x->ip, "127.", 4) != 0 && strcmp(x->ip, "::1") != 0)
        return reply_error(x, 404, NULL);
    size_t cap = 65536;               /* 16 routes x 8 backends fit easily */
    char *out = malloc(cap);
    int n = out ? lb_status_render(out, cap, x->cfg,
                                   __atomic_load_n(&g_busy, __ATOMIC_RELAXED),
                                   queue_count(x->queue),
                                   __atomic_load_n(&g_shed_queue, __ATOMIC_RELAXED),
                                   __atomic_load_n(&g_shed_stale, __ATOMIC_RELAXED))
                : -1;
    if (n < 0) {
        free(out);
        return reply_error(x, 500, NULL);
    }
    reply(x, 200, "text/plain; charset=utf-8", out, (size_t)n, NULL);
    free(out);
    return 200;
}

/* Try backends in route_pick order. A failed connect (dead) or a full
 * in-flight cap (busy) moves on to the next one: once the request was sent
 * it is never retried. Busy is not a health failure, so it is never
 * reported. If no candidate connected and at least one was skipped for the
 * cap, the answer is 503 (retry later) instead of 502. */
static int proxy_route(ctx *x, route *r)
{
    backend *c[ROUTE_MAX_BACKENDS];
    size_t n = route_pick(r, x->ip, c, ROUTE_MAX_BACKENDS);
    int capped = 0;
    for (size_t i = 0; i < n; i++) {
        backend *b = c[i];
        /* Cap 0 still counts in-flight (for the status page) but never sheds. */
        if (!backend_try_acquire(b, x->cfg->backend_cap ? x->cfg->backend_cap : UINT_MAX)) {
            __atomic_fetch_add(&b->shed, 1, __ATOMIC_RELAXED);
            capped = 1;
            continue;
        }
        int up = tcp_connect_timeout(b->host, b->port, x->cfg->proxy_timeout_ms);
        backend_report(b, up >= 0);
        if (up < 0) {
            backend_release(b);
            __atomic_fetch_add(&b->req_err, 1, __ATOMIC_RELAXED);
            continue;
        }
        int st = proxy_reverse_fd(x->fd, up, x->req, b, x->ip);
        backend_release(b);
        __atomic_fetch_add(st == 0 ? &b->req_ok : &b->req_err, 1, __ATOMIC_RELAXED);
        return st;
    }
    return capped ? 503 : 502;
}

/* Returns the status for the access log (0 = relayed by a proxy). */
static int dispatch(ctx *x)
{
    http_request *req = x->req;
    server_config *cfg = x->cfg;

    /* Origin-form paths must be canonical before any routing decision:
     * "/api/../admin" would otherwise match the "/api/" route prefix but
     * mean something else to the backend. "//host" paths are refused so a
     * directory redirect can never emit a protocol-relative Location
     * (open redirect). Checked on the decoded path, so %2e%2e counts. */
    if (req->uri[0] == '/' &&
        (path_has_dot_segment(req->path) || req->path[1] == '/' ||
         req->uri[1] == '/')) {
        x->keep_alive = 0;
        return reply_error(x, 400, NULL);
    }

    if (req->uri[0] == '/' && strcmp(req->path, "/_lb/status") == 0 &&
        (strcmp(req->method, "GET") == 0 || is_head(x)))
        return serve_lb_status(x);

    /* 1. absolute-form target -> forward proxy. Proxied connections are
     *    closed afterwards: the relay streams until the origin closes. */
    if (strncasecmp(req->uri, "http://", 7) == 0) {
        x->keep_alive = 0;
        if (!cfg->forward_proxy)
            return reply_error(x, 400, NULL);
        int st = proxy_forward(x->fd, req, cfg->proxy_timeout_ms, x->ip);
        if (st != 0)
            return reply_error(x, st, NULL);
        return 0;
    }

    /* 2. reverse-proxy routes (longest prefix wins) */
    route *r = route_match(&cfg->routes, req->path);
    if (r != NULL) {
        x->keep_alive = 0;
        int st = proxy_route(x, r);
        if (st != 0)
            return reply_error(x, st, NULL);
        return 0;
    }

    /* 3. built-in dynamic handler, CGI, then static files */
    if (strcmp(req->path, "/echo") == 0)
        return serve_echo(x);
    if (cfg->cgi_dir[0] && strncmp(req->path, "/cgi-bin/", 9) == 0)
        return serve_cgi(x);
    return serve_static(x);
}

static void log_access(const char *ip, const http_request *req, int status)
{
    char ts[32] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) != NULL)
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    /* A single fprintf: POSIX stdio locks the stream per call, so lines
     * from different workers never interleave. */
    if (status > 0)
        fprintf(stderr, "[%s] %s \"%s %s\" %d\n", ts, ip,
                req->method[0] ? req->method : "-",
                req->uri[0] ? req->uri : "-", status);
    else
        fprintf(stderr, "[%s] %s \"%s %s\" proxied\n", ts, ip,
                req->method, req->uri);
}

static void set_timeout(int fd, int opt, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

static void handle_connection(server_config *cfg, client_queue *q,
                              const client *cl)
{
    http_conn *conn = malloc(sizeof *conn);
    http_request *req = malloc(sizeof *req);
    if (conn == NULL || req == NULL) {
        http_send_error(cl->fd, 503, 0);
        free(conn);
        free(req);
        return;
    }
    conn->fd = cl->fd;
    conn->len = 0;

    /* SO_RCVTIMEO doubles as the keep-alive idle timeout. */
    set_timeout(cl->fd, SO_RCVTIMEO, cfg->keepalive_timeout_ms);
    set_timeout(cl->fd, SO_SNDTIMEO, SEND_TIMEOUT_MS);

    while (!g_stop) {
        int rc = http_read_request(conn, req);
        if (rc < 0)
            break;                      /* closed or idle timeout */
        if (rc > 0) {
            /* Malformed / oversized request: answer, then close since the
             * stream position is no longer trustworthy. */
            http_send_error(cl->fd, rc, 0);
            log_access(cl->ip, req, rc);
            http_request_free(req);
            break;
        }
        /* A worker owns its connection for as long as keep-alive lasts.
         * If other clients are queued, answer with "Connection: close" so
         * this worker frees up instead of starving them. */
        ctx x = { cfg, cl->fd, cl->ip, req,
                  req->keep_alive && !g_stop && !queue_has_waiting(q), q };
        int status = dispatch(&x);
        log_access(cl->ip, req, status);
        http_request_free(req);
        if (!x.keep_alive)
            break;
    }
    free(conn);
    free(req);
}

typedef struct {
    server_config *cfg;
    client_queue *queue;
} worker_arg;

static void *worker_main(void *arg)
{
    worker_arg *wa = arg;
    client cl;
    while (queue_pop(wa->queue, &cl) == 0) {
        /* A client queued longer than -W has likely given up; serving it
         * would only burn a worker on a dead request. */
        int wait_max = wa->cfg->queue_wait_max_ms;
        if (wait_max > 0 && now_ms() - cl.accepted_ms > wait_max) {
            set_timeout(cl.fd, SO_SNDTIMEO, 1000);
            http_send_error(cl.fd, 503, 0);
            close(cl.fd);
            __atomic_fetch_add(&g_shed_stale, 1, __ATOMIC_RELAXED);
            fprintf(stderr, "%s rejected: queued too long (503)\n", cl.ip);
            continue;
        }
        __atomic_fetch_add(&g_busy, 1, __ATOMIC_RELAXED);
        handle_connection(wa->cfg, wa->queue, &cl);
        __atomic_fetch_sub(&g_busy, 1, __ATOMIC_RELAXED);
        close(cl.fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* setup + accept loop                                                 */
/* ------------------------------------------------------------------ */

static int open_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);     /* CGI children must not inherit */
    /* SO_REUSEADDR: allow an immediate restart while old connections sit
     * in TIME_WAIT; without it bind() fails with EADDRINUSE. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    /* Non-blocking so accept() after poll() never stalls if the client
     * already went away. Accepted sockets are made blocking again. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static int accept_client(int lfd, client *cl)
{
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
#ifdef __linux__
    int fd = accept4(lfd, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
#else
    int fd = accept(lfd, (struct sockaddr *)&peer, &pl);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        /* BSD/macOS: accepted sockets inherit O_NONBLOCK */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    }
#endif
    if (fd < 0)
        return -1;
    /* We write whole responses at once; no need for Nagle's delay. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (inet_ntop(AF_INET, &peer.sin_addr, cl->ip, sizeof cl->ip) == NULL)
        strcpy(cl->ip, "?");
    cl->fd = fd;
    cl->accepted_ms = now_ms();
    return 0;
}

static void accept_loop(int lfd, client_queue *q)
{
    while (!g_stop) {
        /* poll with a short timeout closes the race where the signal lands
         * between the g_stop check and a blocking accept(). */
        struct pollfd pfd = { lfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 500);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (r == 0)
            continue;

        client cl;
        if (accept_client(lfd, &cl) < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                /* Out of fds: back off instead of spinning. */
                struct timespec ts = { 0, 50 * 1000 * 1000 };
                fprintf(stderr, "accept: %s\n", strerror(errno));
                nanosleep(&ts, NULL);
            } else if (errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK && errno != ECONNABORTED) {
                perror("accept");
            }
            continue;
        }
        if (queue_push(q, &cl) != 0) {
            /* Overloaded: fail fast. */
            set_timeout(cl.fd, SO_SNDTIMEO, 1000);
            http_send_error(cl.fd, 503, 0);
            close(cl.fd);
            __atomic_fetch_add(&g_shed_queue, 1, __ATOMIC_RELAXED);
            fprintf(stderr, "%s rejected: queue full (503)\n", cl.ip);
        }
    }
}

int server_run(server_config *cfg)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    /* Writing to a peer that disconnected must return EPIPE, not kill us. */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* No SA_RESTART: blocking calls in the main thread return EINTR. */
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = open_listener(cfg->port);
    if (lfd < 0)
        return -1;

    client_queue queue;
    if (queue_init(&queue, (size_t)cfg->queue_size) != 0) {
        close(lfd);
        return -1;
    }

    /* Workers block SIGINT/SIGTERM (mask is inherited at creation) so the
     * signal is always delivered to the accepting main thread. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);

    pthread_t *threads = calloc((size_t)cfg->threads, sizeof *threads);
    worker_arg wa = { cfg, &queue };
    int started = 0;
    for (; threads != NULL && started < cfg->threads; started++)
        if (pthread_create(&threads[started], NULL, worker_main, &wa) != 0)
            break;
    /* Created while signals are blocked, like the workers. */
    if (started > 0 && health_start(cfg) != 0)
        fprintf(stderr, "could not start health checker\n");
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (started == 0) {
        fprintf(stderr, "could not start worker threads\n");
    } else {
        fprintf(stderr,
                "mini-webserver listening on :%d  root=%s  cgi=%s  "
                "threads=%d  forward-proxy=%s  routes=%zu\n",
                cfg->port, cfg->root, cfg->cgi_dir[0] ? cfg->cgi_dir : "(off)",
                started, cfg->forward_proxy ? "on" : "off", cfg->routes.count);
        accept_loop(lfd, &queue);
        fprintf(stderr, "shutting down\n");
    }

    /* Stop accepting, wake idle workers, wait for in-flight connections
     * (each finishes its current request; idle keep-alive connections end
     * within keepalive_timeout_ms). */
    close(lfd);
    health_stop();
    queue_close(&queue);
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    free(threads);
    queue_destroy(&queue);
    return started == 0 ? -1 : 0;
}
