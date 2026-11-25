#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t net_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t net_deadline(void)
{
    return net_now_ms() + IO_TIMEOUT_MS;
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

int net_start_connect(const char *ip, uint16_t port)
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
    if (connect(fd, (struct sockaddr *)&address, sizeof address) == -1 && errno != EINPROGRESS)
        return close_with_error(fd);
    return fd;
}
