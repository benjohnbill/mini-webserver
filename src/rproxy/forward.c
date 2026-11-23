#include "forward.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"

#define ORIGIN_PORT_MIN 1
#define MS_PER_SECOND 1000

/* A cursor over the buffer of forward_format_request. */
struct text_cursor {
    char *buffer;
    size_t size;
    size_t used;
    bool is_full;
};

static bool port_is_valid(const char *port);
static void text_append(struct text_cursor *cursor, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void text_append_bytes(struct text_cursor *cursor, const char *bytes,
                              size_t len);
static int origin_connect(const struct origin *origin, int timeout);
static int socket_connect(const struct addrinfo *addr, int timeout);
static bool socket_wait_connect(int origin_fd, int timeout);
static void socket_set_timeouts(int origin_fd, int timeout);
static int origin_send(int origin_fd, const char *bytes, size_t len);
static int origin_read_reply(int origin_fd, char **reply, size_t *reply_len);

int origin_parse(const char *text, struct origin *origin)
{
    const char *colon = strchr(text, ':');
    if (colon == NULL || strchr(colon + 1, ':') != NULL) {
        return -1;
    }
    size_t host_len = (size_t)(colon - text);
    const char *port = colon + 1;
    size_t port_len = strlen(port);
    if (host_len == 0 || host_len >= ORIGIN_HOST_MAX || port_len == 0 ||
        port_len >= ORIGIN_PORT_MAX || !port_is_valid(port)) {
        return -1;
    }
    memcpy(origin->host, text, host_len);
    origin->host[host_len] = '\0';
    memcpy(origin->port, port, port_len + 1);

    return 0;
}

size_t origin_list_pick(struct origin_list *list)
{
    return atomic_fetch_add(&list->next, 1) % list->count;
}

size_t forward_format_request(const struct request *request,
                              const char *client_ip, const char *ja4,
                              char *buffer, size_t size)
{
    struct text_cursor cursor = {.buffer = buffer, .size = size};
    text_append(&cursor, "%s %s %s\r\n", request->method, request->target,
                request->version);
    if (request->host != NULL) {
        text_append(&cursor, "Host: %s\r\n", request->host);
    }
    if (request->user_agent != NULL) {
        text_append(&cursor, "User-Agent: %s\r\n", request->user_agent);
    }
    if (request->content_type != NULL) {
        text_append(&cursor, "Content-Type: %s\r\n", request->content_type);
    }
    if (request->content_length != -1) {
        text_append(&cursor, "Content-Length: %ld\r\n",
                    request->content_length);
    }
    text_append(&cursor,
                "X-Forwarded-For: %s\r\nX-JA4: %s\r\n"
                "Connection: close\r\n\r\n",
                client_ip, ja4);
    text_append_bytes(&cursor, request->body, request->body_len);

    return cursor.is_full ? 0 : cursor.used;
}

int forward_fetch(const struct origin_list *list, size_t first,
                  const char *request_bytes, size_t request_len, int timeout,
                  char **reply, size_t *reply_len)
{
    *reply = NULL;
    *reply_len = 0;
    int origin_fd = origin_connect(&list->origins[first], timeout);
    if (origin_fd < 0 && list->count > 1) {
        origin_fd =
            origin_connect(&list->origins[(first + 1) % list->count], timeout);
    }
    if (origin_fd < 0) {
        return 502;
    }
    int status = origin_send(origin_fd, request_bytes, request_len);
    if (status == 0) {
        status = origin_read_reply(origin_fd, reply, reply_len);
    }
    close(origin_fd);
    if (status != 0) {
        free(*reply);
        *reply = NULL;
        *reply_len = 0;
    }

    return status;
}

static bool port_is_valid(const char *port)
{
    long number = 0;
    for (const char *cursor = port; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        number = number * 10 + (*cursor - '0');
    }

    return number >= ORIGIN_PORT_MIN && number <= PORT_MAX;
}

static void text_append(struct text_cursor *cursor, const char *format, ...)
{
    if (cursor->is_full) {
        return;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(cursor->buffer + cursor->used,
                           cursor->size - cursor->used, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= cursor->size - cursor->used) {
        cursor->is_full = true;
        return;
    }
    cursor->used += (size_t)length;
}

static void text_append_bytes(struct text_cursor *cursor, const char *bytes,
                              size_t len)
{
    if (cursor->is_full || len == 0) {
        return;
    }
    if (len > cursor->size - cursor->used) {
        cursor->is_full = true;
        return;
    }
    memcpy(cursor->buffer + cursor->used, bytes, len);
    cursor->used += len;
}

/* Returns a connected socket with SO_RCVTIMEO and SO_SNDTIMEO set, or -1. */
static int origin_connect(const struct origin *origin, int timeout)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM};
    struct addrinfo *found = NULL;
    if (getaddrinfo(origin->host, origin->port, &hints, &found) != 0) {
        return -1;
    }
    int origin_fd = -1;
    for (struct addrinfo *addr = found; addr != NULL && origin_fd < 0;
         addr = addr->ai_next) {
        origin_fd = socket_connect(addr, timeout);
    }
    freeaddrinfo(found);
    if (origin_fd >= 0) {
        socket_set_timeouts(origin_fd, timeout);
    }

    return origin_fd;
}

/* A non-blocking connect, so that a connect that hangs ends in timeout
   seconds. The socket is blocking again on return. */
static int socket_connect(const struct addrinfo *addr, int timeout)
{
    int origin_fd =
        socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (origin_fd < 0) {
        return -1;
    }
    int flags = fcntl(origin_fd, F_GETFL, 0);
    fcntl(origin_fd, F_SETFL, flags | O_NONBLOCK);
    bool is_connected =
        connect(origin_fd, addr->ai_addr, addr->ai_addrlen) == 0 ||
        (errno == EINPROGRESS && socket_wait_connect(origin_fd, timeout));
    if (!is_connected) {
        close(origin_fd);
        return -1;
    }
    fcntl(origin_fd, F_SETFL, flags);

    return origin_fd;
}

static bool socket_wait_connect(int origin_fd, int timeout)
{
    struct pollfd watch = {.fd = origin_fd, .events = POLLOUT};
    int ready = poll(&watch, 1, timeout * MS_PER_SECOND);
    if (ready <= 0) {
        return false;
    }
    int error = 0;
    socklen_t error_len = sizeof(error);
    getsockopt(origin_fd, SOL_SOCKET, SO_ERROR, &error, &error_len);

    return error == 0;
}

static void socket_set_timeouts(int origin_fd, int timeout)
{
    struct timeval limit = {.tv_sec = timeout, .tv_usec = 0};
    setsockopt(origin_fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(origin_fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

static int origin_send(int origin_fd, const char *bytes, size_t len)
{
    for (size_t sent = 0; sent < len;) {
        ssize_t written = write(origin_fd, bytes + sent, len - sent);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 504 : 502;
        }
        sent += (size_t)written;
    }

    return 0;
}

/* Reads to EOF. An empty reply is 502. */
static int origin_read_reply(int origin_fd, char **reply, size_t *reply_len)
{
    for (;;) {
        char *data = realloc(*reply, *reply_len + PROXY_READ_CHUNK);
        if (data == NULL) {
            return 502;
        }
        *reply = data;
        ssize_t length = read(origin_fd, *reply + *reply_len, PROXY_READ_CHUNK);
        if (length == 0) {
            return *reply_len > 0 ? 0 : 502;
        }
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 504 : 502;
        }
        *reply_len += (size_t)length;
        if (*reply_len > PROXY_REPLY_MAX) {
            return 502;
        }
    }
}
