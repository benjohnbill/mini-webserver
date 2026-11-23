#include "proxy.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int upstream_connect(const struct config *config);
static int upstream_send_request(int upstream_fd, const struct request *request,
                                 const struct config *config);
static int upstream_read_reply(int upstream_fd, struct proxy_reply *reply);

int proxy_fetch(const struct request *request, const struct config *config,
                struct proxy_reply *reply)
{
    *reply = (struct proxy_reply){0};
    int upstream_fd = upstream_connect(config);
    if (upstream_fd < 0) {
        return 502;
    }
    int status = upstream_send_request(upstream_fd, request, config);
    if (status == 0) {
        status = upstream_read_reply(upstream_fd, reply);
    }
    close(upstream_fd);
    if (status != 0) {
        proxy_reply_destroy(reply);
    }

    return status;
}

void proxy_reply_destroy(struct proxy_reply *reply)
{
    free(reply->data);
    reply->data = NULL;
    reply->len = 0;
}

/* Returns a connected socket with SO_RCVTIMEO set, or -1. */
static int upstream_connect(const struct config *config)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM};
    struct addrinfo *found = NULL;
    if (getaddrinfo(config->upstream_host, config->upstream_port, &hints,
                    &found) != 0) {
        return -1;
    }
    int upstream_fd = -1;
    for (struct addrinfo *addr = found; addr != NULL; addr = addr->ai_next) {
        upstream_fd =
            socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (upstream_fd >= 0 &&
            connect(upstream_fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            break;
        }
        if (upstream_fd >= 0) {
            close(upstream_fd);
            upstream_fd = -1;
        }
    }
    freeaddrinfo(found);
    if (upstream_fd >= 0) {
        struct timeval timeout = {.tv_sec = config->timeout, .tv_usec = 0};
        setsockopt(upstream_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
    }

    return upstream_fd;
}

/* HTTP/1.0 and Connection: close, so the upstream ends the body with EOF
   and does not use chunked transfer. */
static int upstream_send_request(int upstream_fd, const struct request *request,
                                 const struct config *config)
{
    char text[PROXY_REQUEST_MAX];
    const char *target = request->target + strlen(PROXY_PREFIX) - 1;
    int length = snprintf(text, sizeof(text),
                          "%s %s HTTP/1.0\r\nHost: %s:%s\r\n"
                          "Connection: close\r\n\r\n",
                          request->method, target, config->upstream_host,
                          config->upstream_port);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        return 502;
    }
    for (size_t sent = 0; sent < (size_t)length;) {
        ssize_t written =
            write(upstream_fd, text + sent, (size_t)length - sent);
        if (written < 0 && errno != EINTR) {
            return 502;
        }
        sent += written > 0 ? (size_t)written : 0;
    }

    return 0;
}

static int upstream_read_reply(int upstream_fd, struct proxy_reply *reply)
{
    for (;;) {
        char *data = realloc(reply->data, reply->len + PROXY_READ_CHUNK);
        if (data == NULL) {
            return 502;
        }
        reply->data = data;
        ssize_t length =
            read(upstream_fd, reply->data + reply->len, PROXY_READ_CHUNK);
        if (length == 0) {
            return reply->len > 0 ? 0 : 502;
        }
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 504 : 502;
        }
        reply->len += (size_t)length;
        if (reply->len > PROXY_REPLY_MAX) {
            return 502;
        }
    }
}
