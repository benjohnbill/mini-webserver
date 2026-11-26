#include "connection.h"
#include "proxy_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum Phase {
    READ_REQUEST, CONNECT_BACKEND, SEND_BACKEND_HEAD, READ_UPLOAD, SEND_BACKEND_BODY, READ_BACKEND_HEAD,
    SEND_CLIENT_HEAD, SEND_CLIENT_BODY, READ_BACKEND_BODY, DRAIN_CLIENT, FINISHED
};
enum Body { EMPTY, ECHO, FILE_BODY, BACKEND_BODY, CACHED_BODY };

struct Connection {
    const ServerConfig *config;
    int client, backend, file;
    enum Phase phase;
    enum Body body;
    int64_t deadline, backend_deadline;
    int64_t paused_budget_ms;
    bool drain_after_error;
    Request request;
    UpstreamResponse upstream;
    HttpOutput header;
    unsigned char chunk[RELAY_CHUNK];
    CacheTicket cache;
    int64_t request_started;
    bool capture;
    size_t buffered, remaining;
    const unsigned char *pending;
    size_t unsent;
};

Connection *connection_new(int client, const ServerConfig *config)
{
    Connection *c = calloc(1, sizeof *c);
    if (c != NULL) {
        c->config = config;
        c->client = client;
        c->backend = c->file = -1;
        c->deadline = net_deadline();
        c->phase = READ_REQUEST;
    }
    return c;
}

void connection_free(Connection *c)
{
    if (c->backend >= 0)
        close(c->backend);
    if (c->file >= 0)
        close(c->file);
    free(c->request.body);
    free(c->request.path);
    free(c);
}

struct pollfd connection_interest(const Connection *c)
{
    struct pollfd wait = {.fd = c->client, .events = POLLIN};
    switch (c->phase) {
    case CONNECT_BACKEND: case SEND_BACKEND_HEAD: case SEND_BACKEND_BODY:
        wait.fd = c->backend;
        wait.events = POLLOUT;
        break;
    case READ_BACKEND_HEAD: case READ_BACKEND_BODY:
        wait.fd = c->backend;
        break;
    case SEND_CLIENT_HEAD: case SEND_CLIENT_BODY:
        wait.events = POLLOUT;
        break;
    case FINISHED:
        wait.fd = -1;
        break;
    default:
        break;
    }
    return wait;
}

int64_t connection_deadline(const Connection *c)
{
    return c->deadline;
}

static void queue_write(Connection *c, enum Phase phase, const void *data, size_t length)
{
    c->phase = phase;
    c->pending = data;
    c->unsent = length;
}

static void client_header(Connection *c, enum Body body)
{
    c->body = body;
    c->deadline = net_deadline();
    queue_write(c, SEND_CLIENT_HEAD, c->header.data, c->header.length);
}

static void error_response(Connection *c, int status, const char *allow)
{
    if (c->backend >= 0)
        shutdown(c->backend, SHUT_RDWR);
    if (c->request.received < c->request.head.length)
        c->drain_after_error = true;
    if (http_make_error(&c->header, status, allow) < 0)
        c->phase = FINISHED;
    else
        client_header(c, EMPTY);
}

/* Charge only the peer we are waiting on. Switching sockets never renews
   either peer's timeout, even when a body arrives a byte at a time. */
static void switch_upload_clock(Connection *c)
{
    int64_t now = net_now_ms();
    int64_t left = c->deadline - now;
    c->deadline = now + c->paused_budget_ms;
    c->paused_budget_ms = left;
}

static void begin_proxy(Connection *c, const Route *route, const unsigned char *body, size_t size)
{
    struct sockaddr_in peer = {0};
    if (route->count > 1 || c->config->rate) {
        socklen_t length = sizeof peer;
        if (getpeername(c->client, (struct sockaddr *)&peer, &length) < 0 ||
            peer.sin_family != AF_INET) {
            error_response(c, 500, NULL);
            return;
        }
    }
    if (c->config->rate && !proxy_rate_allow(c->config->policy, peer.sin_addr,
                                             c->config->rate, c->config->burst)) {
        error_response(c, 429, NULL);
        return;
    }
    const Backend *backend = ketama_select(route, peer.sin_addr);
    if (c->config->cache_enabled && proxy_cache_key(&c->cache, &c->request, backend) &&
        proxy_cache_get(c->config->policy, &c->cache, &c->header, c->chunk, &c->buffered)) {
        client_header(c, CACHED_BODY);
        return;
    }
    c->request_started = net_now_ms();
    /* A recv of the header may already include part (or all) of the body. */
    c->buffered = size < c->request.head.length ? size : c->request.head.length;
    memmove(c->chunk, body, c->buffered);
    c->request.received = c->buffered;
    c->paused_budget_ms = IO_TIMEOUT_MS;
    switch_upload_clock(c);
    c->backend = net_start_connect(backend->ip, backend->port);
    if (c->backend < 0)
        error_response(c, errno == ETIMEDOUT ? 504 : 502, NULL);
    else if (build_proxy_request(&c->header, backend, &c->request) < 0)
        error_response(c, 500, NULL);
    else
        c->phase = CONNECT_BACKEND;
}

static void dispatch_local(Connection *c)
{
    Request *req = &c->request;
    bool get = strcmp(req->method, "GET") == 0;
    bool post = strcmp(req->method, "POST") == 0;
    if (strcmp(req->path, "/echo") == 0) {
        if (!post)
            error_response(c, 405, "POST");
        else if (http_make_head(&c->header, 200, "application/octet-stream", req->head.length, NULL) < 0)
            error_response(c, 500, NULL);
        else
            client_header(c, ECHO);
    } else if (get) {
        const char *type;
        int status = open_static(c->config->root_fd, req->path, &c->file, &c->remaining, &type);
        if (status != 0)
            error_response(c, status, NULL);
        else if (http_make_head(&c->header, 200, type, c->remaining, NULL) < 0)
            error_response(c, 500, NULL);
        else
            client_header(c, FILE_BODY);
    } else {
        error_response(c, 405, match_route(c->config, req->path) ? "GET, POST" : "GET");
    }
}

static bool retry_io(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void read_request(Connection *c)
{
    Request *req = &c->request;
    size_t limit = sizeof c->chunk;
    if (c->phase == READ_UPLOAD && req->head.length - req->received < limit)
        limit = req->head.length - req->received;
    ssize_t n = recv(c->client, c->chunk, limit, 0);
    if (n < 0 && retry_io())
        return;
    if (n == 0 && c->request.head.used == 0) {
        c->phase = FINISHED;
        return;
    }
    if (n <= 0) {
        c->drain_after_error = true;
        error_response(c, 400, NULL);
        return;
    }
    const unsigned char *data = c->chunk;
    size_t size = (size_t)n;
    if (c->phase == READ_UPLOAD) {
        req->received += size;
        switch_upload_clock(c);
        queue_write(c, SEND_BACKEND_BODY, data, size);
        return;
    }
    if (req->head.first_line == NULL) {
        int status = http_feed_request_head(req, &data, &size);
        if (status != 0) {
            if (status != HTTP_MORE) {
                c->drain_after_error = true;
                error_response(c, status, NULL);
            }
            return;
        }
        const Route *route = match_route(c->config, req->path);
        if (route && strcmp(req->path, "/echo") != 0 &&
            (strcmp(req->method, "GET") == 0 || strcmp(req->method, "POST") == 0)) {
            begin_proxy(c, route, data, size);
            return;
        }
    }
    int status = http_feed_request(req, data, size);
    if (status == 0)
        dispatch_local(c);
    else if (status != HTTP_MORE) {
        c->drain_after_error = true;
        error_response(c, status, NULL);
    }
}

static void read_backend_head(Connection *c)
{
    ssize_t n = recv(c->backend, c->chunk, sizeof c->chunk, 0);
    if (n < 0 && retry_io())
        return;
    if (n <= 0) {
        error_response(c, 502, NULL);
        return;
    }
    const unsigned char *data = c->chunk;
    size_t size = (size_t)n;
    while (size > 0) {
        UpstreamResponse *r = &c->upstream;
        int status = http_feed_response(r, &data, &size);
        if (status == HTTP_MORE)
            return;
        if (status != 0 || r->status == 101) {
            error_response(c, 502, NULL);
            return;
        }
        if (r->status < 200) {
            memset(r, 0, sizeof *r);
            continue;
        }
        if (r->head.has_transfer_encoding || build_proxy_response(&c->header, r) < 0) {
            error_response(c, 502, NULL);
            return;
        }
        if (c->config->cache_enabled && !strcmp(c->request.method, "POST") && r->status < 400)
            proxy_cache_invalidate(c->config->policy);
        c->capture = c->header.length <= CACHE_HEADER_SIZE &&
                     proxy_cache_admit(&c->cache, r, c->request_started);
        c->remaining = r->head.has_length ? r->head.length : SIZE_MAX;
        c->buffered = size < c->remaining ? size : c->remaining;
        memmove(c->chunk, data, c->buffered);
        if (c->capture && !c->remaining)
            proxy_cache_put(c->config->policy, &c->cache, &c->header, c->chunk, 0);
        client_header(c, r->status == 204 || r->status == 304 ? EMPTY : BACKEND_BODY);
        return;
    }
}

static void send_chunk(Connection *c, size_t size)
{
    size_t offset = c->capture ? c->upstream.head.length - c->remaining : 0;
    if (c->body == FILE_BODY || c->upstream.head.has_length)
        c->remaining -= size;
    if (c->capture && !c->remaining)
        proxy_cache_put(c->config->policy, &c->cache, &c->header, c->chunk, c->upstream.head.length);
    queue_write(c, SEND_CLIENT_BODY, c->chunk + offset, size);
}

static void next_body(Connection *c)
{
    switch (c->body) {
    case CACHED_BODY:
        c->body = EMPTY;
        if (c->buffered) {
            queue_write(c, SEND_CLIENT_BODY, c->chunk, c->buffered);
            return;
        }
        break;
    case ECHO:
        c->body = EMPTY;
        if (c->request.head.length > 0) {
            queue_write(c, SEND_CLIENT_BODY, c->request.body, c->request.head.length);
            return;
        }
        break;
    case FILE_BODY:
        if (c->remaining > 0) {
            size_t size = c->remaining < sizeof c->chunk ? c->remaining : sizeof c->chunk;
            ssize_t n;
            do {
                n = read(c->file, c->chunk, size);
            } while (n < 0 && errno == EINTR);
            if (n > 0) {
                send_chunk(c, (size_t)n);
                return;
            }
        }
        break;
    case BACKEND_BODY:
        if (c->buffered > 0) {
            send_chunk(c, c->buffered);
            c->buffered = 0;
            return;
        }
        if (c->remaining > 0) {
            c->phase = READ_BACKEND_BODY;
            if (c->backend_deadline < c->deadline)
                c->deadline = c->backend_deadline;
            return;
        }
        break;
    case EMPTY:
        if (c->drain_after_error && shutdown(c->client, SHUT_WR) == 0) {
            c->deadline = net_now_ms() + 200;
            c->phase = DRAIN_CLIENT;
            return;
        }
        break;
    }
    c->phase = FINISHED;
}

static void write_pending(Connection *c)
{
    int fd = connection_interest(c).fd;
    ssize_t n = send(fd, c->pending, c->unsent, 0);
    if (n < 0 && retry_io())
        return;
    if (n <= 0) {
        if (c->phase == SEND_BACKEND_HEAD || c->phase == SEND_BACKEND_BODY)
            error_response(c, 502, NULL);
        else
            c->phase = FINISHED;
        return;
    }
    c->pending += n;
    c->unsent -= (size_t)n;
    if (c->unsent > 0)
        return;
    if (c->phase == SEND_BACKEND_HEAD || c->phase == SEND_BACKEND_BODY) {
        if (c->buffered > 0) {
            queue_write(c, SEND_BACKEND_BODY, c->chunk, c->buffered);
            c->buffered = 0;
        } else if (c->request.received < c->request.head.length) {
            switch_upload_clock(c);
            c->phase = READ_UPLOAD;
        } else {
            c->backend_deadline = c->deadline;
            c->phase = READ_BACKEND_HEAD;
        }
    } else {
        next_body(c);
    }
}

void connection_step(Connection *c)
{
    switch (c->phase) {
    case READ_REQUEST: case READ_UPLOAD:
        read_request(c);
        break;
    case CONNECT_BACKEND: {
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(c->backend, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
            error_response(c, error == ETIMEDOUT ? 504 : 502, NULL);
        else
            queue_write(c, SEND_BACKEND_HEAD, c->header.data, c->header.length);
        break;
    }
    case READ_BACKEND_HEAD:
        read_backend_head(c);
        break;
    case READ_BACKEND_BODY: {
        size_t size = c->remaining < sizeof c->chunk ? c->remaining : sizeof c->chunk;
        /* An admitted small reply fits in the relay buffer. Keep its prefix
           for the cache while still sending each newly read slice immediately. */
        size_t offset = c->capture ? c->upstream.head.length - c->remaining : 0;
        ssize_t n = recv(c->backend, c->chunk + offset, size, 0);
        if (n > 0)
            send_chunk(c, (size_t)n);
        else if (n == 0 || !retry_io())
            c->phase = FINISHED;
        break;
    }
    case DRAIN_CLIENT: {
        ssize_t n = recv(c->client, c->chunk, sizeof c->chunk, 0);
        if (n == 0 || (n < 0 && !retry_io()))
            c->phase = FINISHED;
        break;
    }
    case SEND_BACKEND_HEAD: case SEND_BACKEND_BODY: case SEND_CLIENT_HEAD: case SEND_CLIENT_BODY:
        write_pending(c);
        break;
    case FINISHED:
        break;
    }
}

void connection_timeout(Connection *c)
{
    switch (c->phase) {
    case READ_REQUEST: case READ_UPLOAD:
        c->drain_after_error = true;
        error_response(c, 408, NULL);
        break;
    case CONNECT_BACKEND: case SEND_BACKEND_HEAD: case SEND_BACKEND_BODY: case READ_BACKEND_HEAD:
        error_response(c, 504, NULL);
        break;
    default:
        c->phase = FINISHED;
        break;
    }
}

/* A worker or forked child can wait exclusively for this one connection. */
void handle_client(int fd, const ServerConfig *config)
{
    Connection *c = connection_new(fd, config);
    if (c == NULL)
        return;
    for (;;) {
        struct pollfd wait = connection_interest(c);
        if (wait.fd < 0)
            break;
        int64_t delay = c->deadline - net_now_ms();
        if (delay <= 0) {
            connection_timeout(c);
            continue;
        }
        int result = poll(&wait, 1, (int)delay);
        if (result > 0)
            connection_step(c);
        else if (result < 0 && errno != EINTR)
            break;
    }
    connection_free(c);
}
