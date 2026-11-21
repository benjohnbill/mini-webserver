#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t net_deadline(void)
{
    return now_ms() + IO_TIMEOUT_MS;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
    struct pollfd item = {.fd = fd, .events = events};
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int ready = poll(&item, 1, (int)remaining);
        if (ready > 0)
            return 0;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR)
            return -1;
    }
}

int net_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int close_with_error(int fd)
{
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

int net_listen(const char *ip, uint16_t port)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) == -1 ||
        net_nonblocking(fd) == -1 ||
        bind(fd, (struct sockaddr *)&address, sizeof address) == -1 ||
        listen(fd, SOMAXCONN) == -1)
        return close_with_error(fd);
    return fd;
}

int net_connect(const char *ip, uint16_t port, int64_t deadline)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;
    if (net_nonblocking(fd) == -1)
        return close_with_error(fd);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0)
        return fd;
    if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, deadline) == -1)
        return close_with_error(fd);

    int error;
    socklen_t length = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1)
        return close_with_error(fd);
    if (error != 0) {
        errno = error;
        return close_with_error(fd);
    }
    return fd;
}

void stream_init(Stream *stream, int fd, int64_t deadline)
{
    stream->fd = fd;
    stream->deadline = deadline;
    stream->start = 0;
    stream->end = 0;
}

ssize_t stream_read(Stream *stream, void *buffer, size_t size)
{
    for (;;) {
        if (size == 0)
            return 0;
        if (now_ms() >= stream->deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t bytes = recv(stream->fd, buffer, size, 0);
        if (bytes >= 0)
            return bytes;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (wait_fd(stream->fd, POLLIN, stream->deadline) == -1)
            return -1;
    }
}

int stream_send(Stream *stream, const void *buffer, size_t size)
{
    const unsigned char *next = buffer;
    while (size > 0) {
        if (now_ms() >= stream->deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t bytes = send(stream->fd, next, size, 0);
        if (bytes > 0) {
            next += bytes;
            size -= (size_t)bytes;
            continue;
        }
        if (bytes == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (wait_fd(stream->fd, POLLOUT, stream->deadline) == -1)
            return -1;
    }
    return 0;
}

void net_finish_error(int fd)
{
    /* A client may still be sending the oversized or invalid request. */
    if (shutdown(fd, SHUT_WR) == -1)
        return;
    Stream incoming;
    stream_init(&incoming, fd, now_ms() + 200);
    unsigned char discard[4096];
    while (stream_read(&incoming, discard, sizeof discard) > 0)
        ;
}
