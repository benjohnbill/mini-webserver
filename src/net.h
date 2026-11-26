#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define IO_TIMEOUT_MS 5000

int net_nonblocking(int fd);
int net_listen(const char *ip, uint16_t port);
int net_start_connect(const char *ip, uint16_t port);
int64_t net_now_ms(void);
int64_t net_deadline(void);

#endif
