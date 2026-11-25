#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_for_connect(int fd, int timeout_ms)
{
    int64_t start = monotonic_ms();
    if (start < 0)
        return -1;
    int64_t deadline = start + timeout_ms;
    struct pollfd event = {.fd = fd, .events = POLLOUT};
    int ready;
    for (;;) {
        ready = poll(&event, 1, timeout_ms);
        if (ready >= 0 || errno != EINTR)
            break;
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline)
            return -1;
        timeout_ms = (int)(deadline - now);
    }

    if (ready == 0) {
        errno = ETIMEDOUT; /* lets the caller answer 504 instead of 502 */
        return -1;
    }
    int error;
    socklen_t length = sizeof error;
    if (ready < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        return -1;
    return error == 0 ? 0 : -1;
}

static int connect_address(int fd, const struct addrinfo *address, int timeout_ms)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    if (connect(fd, address->ai_addr, address->ai_addrlen) < 0) {
        if (errno != EINPROGRESS && errno != EINTR)
            return -1;
        if (wait_for_connect(fd, timeout_ms) < 0)
            return -1;
    }

    struct timeval timeout = {.tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000};
    if (fcntl(fd, F_SETFL, flags) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0)
        return -1;
    return 0;
}

int tcp_connect_timeout(const char *host, const char *port, int timeout_ms)
{
    if (timeout_ms <= 0)
        return -1;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses;
    if (getaddrinfo(host, port, &hints, &addresses) != 0)
        return -1;

    int connected = -1, timed_out = 0;
    for (const struct addrinfo *a = addresses; a; a = a->ai_next) {
        /* CLOEXEC: a concurrent CGI fork must not inherit this socket. */
#ifdef SOCK_CLOEXEC
        int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
#else
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
        if (fd < 0)
            continue;
        if (connect_address(fd, a, timeout_ms) == 0) {
            connected = fd;
            break;
        }
        if (errno == ETIMEDOUT)
            timed_out = 1;
        close(fd);
    }
    freeaddrinfo(addresses);
    if (connected < 0)
        errno = timed_out ? ETIMEDOUT : ECONNREFUSED;
    return connected;
}

static int valid_port(const char *start, const char *end)
{
    unsigned int number = 0;
    for (const char *p = start; p < end; ++p) {
        if (*p < '0' || *p > '9')
            return 0;
        number = number * 10 + (unsigned int)(*p - '0');
        if (number > 65535)
            return 0;
    }
    return number != 0;
}

int parse_absolute_uri(const char *uri, char *host, size_t host_size,
                       char *port, size_t port_size,
                       char *path, size_t path_size)
{
    if (!uri || strncasecmp(uri, "http://", sizeof "http://" - 1) != 0)
        return -1;

    const char *host_start = uri + sizeof "http://" - 1;
    const char *authority_end = host_start + strcspn(host_start, "/?");
    if (memchr(host_start, '@', (size_t)(authority_end - host_start)))
        return -1;
    for (const unsigned char *p = (const unsigned char *)host_start; *p; ++p) {
        if (*p <= ' ' || *p == 127 || *p == '#' || *p == '\\')
            return -1;
    }

    const char *host_end = authority_end;
    const char *port_start = authority_end;
    int bracketed = *host_start == '[';
    if (bracketed) {
        ++host_start;
        host_end = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (!host_end)
            return -1;
        port_start = host_end + 1;
        if (port_start != authority_end && *port_start != ':')
            return -1;
    } else {
        const char *separator = memchr(host_start, ':', (size_t)(authority_end - host_start));
        if (separator)
            host_end = port_start = separator;
        if (memchr(host_start, '[', (size_t)(host_end - host_start)) ||
            memchr(host_start, ']', (size_t)(host_end - host_start)))
            return -1;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= host_size)
        return -1;
    if (bracketed) {
        char literal[INET6_ADDRSTRLEN];
        struct in6_addr addr;
        if (host_len >= sizeof literal)
            return -1;
        memcpy(literal, host_start, host_len);
        literal[host_len] = '\0';
        if (inet_pton(AF_INET6, literal, &addr) != 1)
            return -1;
    }

    size_t port_len = 2;
    if (port_start == authority_end) {
        port_start = "80";
    } else {
        ++port_start;
        port_len = (size_t)(authority_end - port_start);
        if (!valid_port(port_start, authority_end))
            return -1;
    }

    /* A query-only target still needs a slash in origin form. */
    size_t leading_slash = *authority_end != '/';
    size_t path_len = strlen(authority_end);
    if (port_len >= port_size || path_len + leading_slash >= path_size)
        return -1;

    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    memcpy(port, port_start, port_len);
    port[port_len] = '\0';
    if (leading_slash)
        path[0] = '/';
    memcpy(path + leading_slash, authority_end, path_len + 1);
    return 0;
}

static int is_hop_by_hop(const http_request *req, const char *name)
{
    static const char *const names[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authorization",
        "TE", "Trailer", "Transfer-Encoding", "Upgrade"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (strcasecmp(name, names[i]) == 0)
            return 1;
    }

    size_t name_len = strlen(name);
    for (size_t i = 0; i < req->header_count; ++i) {
        const http_header *header = &req->headers[i];
        if (strcasecmp(header->name, "Connection") != 0)
            continue;
        const char *token = header->value;
        while (*token) {
            const char *comma = token + strcspn(token, ",");
            token += strspn(token, " \t");
            const char *end = comma;
            while (end > token && (end[-1] == ' ' || end[-1] == '\t'))
                --end;
            if ((size_t)(end - token) == name_len &&
                strncasecmp(token, name, name_len) == 0)
                return 1;
            token = *comma ? comma + 1 : comma;
        }
    }
    return 0;
}

static int append_format(char **out, size_t *remaining, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(*out, *remaining, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= *remaining)
        return -1;
    *out += written;
    *remaining -= (size_t)written;
    return 0;
}

long proxy_build_request(const http_request *req, const char *target,
                         const char *host_header, const char *client_ip,
                         char *out, size_t out_size)
{
    if (out_size == 0 || req->header_count > HTTP_MAX_HEADERS ||
        (req->body_len > 0 && !req->body))
        return -1;

    char *cursor = out;
    size_t remaining = out_size;
    if (append_format(&cursor, &remaining, "%s %s HTTP/1.1\r\n", req->method, target) < 0)
        return -1;
    for (size_t i = 0; i < req->header_count; ++i) {
        const http_header *header = &req->headers[i];
        if (strcasecmp(header->name, "Host") == 0 ||
            strcasecmp(header->name, "Content-Length") == 0 ||
            (client_ip && strcasecmp(header->name, "X-Forwarded-For") == 0) ||
            is_hop_by_hop(req, header->name))
            continue;
        /* Defence in depth against header injection, independent of the parser. */
        if (strpbrk(header->name, "\r\n") || strpbrk(header->value, "\r\n"))
            return -1;
        if (append_format(&cursor, &remaining, "%s: %s\r\n", header->name, header->value) < 0)
            return -1;
    }

    if (append_format(&cursor, &remaining, "Host: %s\r\nConnection: close\r\n", host_header) < 0)
        return -1;
    if (client_ip) {
        if (append_format(&cursor, &remaining, "X-Forwarded-For: ") < 0)
            return -1;
        /* Preserve the order of any incoming X-Forwarded-For fields. */
        if (!is_hop_by_hop(req, "X-Forwarded-For")) {
            for (size_t i = 0; i < req->header_count; ++i) {
                const http_header *header = &req->headers[i];
                if (strcasecmp(header->name, "X-Forwarded-For") == 0 && header->value[0] &&
                    append_format(&cursor, &remaining, "%s, ", header->value) < 0)
                    return -1;
            }
        }
        if (append_format(&cursor, &remaining, "%s\r\n", client_ip) < 0)
            return -1;
    }
    if ((req->body_len > 0 || strcmp(req->method, "POST") == 0) &&
        append_format(&cursor, &remaining, "Content-Length: %zu\r\n", req->body_len) < 0)
        return -1;

    if (remaining < 2 || req->body_len > remaining - 2)
        return -1;
    size_t length = out_size - remaining + 2 + req->body_len;
    if (length > LONG_MAX)
        return -1;
    memcpy(cursor, "\r\n", 2);
    if (req->body_len)
        memcpy(cursor + 2, req->body, req->body_len);
    if (length < out_size)
        out[length] = '\0';
    return (long)length;
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, buf, len, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0) {
            if (sent == 0)
                errno = EIO;
            return -1;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static int upstream_error(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK ? 504 : 502;
}

static int relay_response(int client_fd, int upstream)
{
    char buf[8192];
    int relayed = 0;
    for (;;) {
        ssize_t received = recv(upstream, buf, sizeof buf, 0);
        if (received < 0 && errno == EINTR)
            continue;
        /* Once a response starts, the caller cannot send a second status. */
        if (received <= 0)
            return relayed ? 0 : (received < 0 ? upstream_error() : 502);
        if (send_all(client_fd, buf, (size_t)received) < 0)
            return 0;
        relayed = 1;
    }
}

/* Send req on an already connected upstream fd, relay the response, close
 * the fd. */
static int send_and_relay(int client_fd, int upstream, const http_request *req,
                          const backend *b, const char *target, int include_port,
                          const char *client_ip)
{
    char host_header[sizeof b->host + sizeof b->port + 3];
    int ipv6 = strchr(b->host, ':') != NULL;
    int host_len = snprintf(host_header, sizeof host_header, "%s%s%s%s%s",
                            ipv6 ? "[" : "", b->host, ipv6 ? "]" : "",
                            include_port ? ":" : "", include_port ? b->port : "");
    int status = 502;
    size_t capacity = HTTP_MAX_HEAD_BYTES * 2 + HTTP_MAX_BODY;
    char *request = host_len < 0 || (size_t)host_len >= sizeof host_header
                        ? NULL : malloc(capacity);
    if (request) {
        long length = proxy_build_request(req, target, host_header, client_ip, request, capacity);
        if (length >= 0) {
            status = send_all(upstream, request, (size_t)length) == 0 ? 0 : upstream_error();
        }
        free(request);
    }
    if (status == 0)
        status = relay_response(client_fd, upstream);
    close(upstream);
    return status;
}

static int proxy_to_backend(int client_fd, const http_request *req, const backend *b,
                            const char *target, int include_port, int timeout_ms,
                            const char *client_ip)
{
    int upstream = tcp_connect_timeout(b->host, b->port, timeout_ms);
    if (upstream < 0)
        return errno == ETIMEDOUT ? 504 : 502;
    return send_and_relay(client_fd, upstream, req, b, target, include_port, client_ip);
}

int proxy_reverse_fd(int client_fd, int upstream, const http_request *req,
                     const backend *b, const char *client_ip)
{
    return send_and_relay(client_fd, upstream, req, b, req->uri, 1, client_ip);
}

int proxy_reverse(int client_fd, const http_request *req, const backend *b,
                  int timeout_ms, const char *client_ip)
{
    return proxy_to_backend(client_fd, req, b, req->uri, 1, timeout_ms, client_ip);
}

int proxy_forward(int client_fd, const http_request *req, int timeout_ms,
                  const char *client_ip)
{
    backend b;
    char path[HTTP_MAX_URI];
    if (parse_absolute_uri(req->uri, b.host, sizeof b.host, b.port, sizeof b.port,
                           path, sizeof path) < 0)
        return 400;
    return proxy_to_backend(client_fd, req, &b, path, strcmp(b.port, "80") != 0,
                            timeout_ms, client_ip);
}
