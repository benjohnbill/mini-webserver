#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define IO_TIMEOUT_MS 5000

typedef struct {
    int fd;
    int64_t deadline;
    unsigned char buffer[4096];
    size_t start;
    size_t end;
} Stream;

int net_nonblocking(int fd);
int net_listen(const char *ip, uint16_t port);
int net_connect(const char *ip, uint16_t port, int64_t deadline);
int64_t net_deadline(void);
void net_finish_error(int fd);
void stream_init(Stream *stream, int fd, int64_t deadline);
ssize_t stream_read(Stream *stream, void *buffer, size_t size);
int stream_send(Stream *stream, const void *buffer, size_t size);

#endif
