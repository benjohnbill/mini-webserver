#include "forward.h"

#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"

#define ORIGIN_PORT_MIN 1

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

int origin_resolve(struct origin *origin)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM};
    struct addrinfo *found = NULL;
    int error = getaddrinfo(origin->host, origin->port, &hints, &found);
    if (error != 0) {
        return error;
    }
    /* The first address only, so that one Origin is one address. */
    memcpy(&origin->address, found->ai_addr, found->ai_addrlen);
    origin->address_len = found->ai_addrlen;
    freeaddrinfo(found);

    return 0;
}

int origin_connect(const struct origin *origin)
{
    const struct sockaddr *address = (const struct sockaddr *)&origin->address;
    int origin_fd = socket(address->sa_family,
                           SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (origin_fd < 0) {
        return -1;
    }
    if (connect(origin_fd, address, origin->address_len) != 0 &&
        errno != EINPROGRESS) {
        close(origin_fd);
        return -1;
    }

    return origin_fd;
}

bool origin_is_connected(int origin_fd)
{
    int error = 0;
    socklen_t error_len = sizeof(error);
    int result =
        getsockopt(origin_fd, SOL_SOCKET, SO_ERROR, &error, &error_len);

    return result == 0 && error == 0;
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
