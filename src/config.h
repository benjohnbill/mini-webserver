#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define HEADER_MAX 8192
#define TARGET_MAX 2048
#define BODY_MAX 1048576
#define POOL_QUEUE_MAX 64
#define EPOLL_WAIT_MS 1000
#define EPOLL_EVENTS_MAX 64
#define SEND_CHUNK_SIZE 16384
#define PATH_BUFFER_SIZE 4096
#define PORT_MAX 65535
#define WORKERS_MAX 1024
#define TIMEOUT_MAX 3600

#define DEFAULT_MODEL "pool"
#define DEFAULT_WORKERS 4
#define DEFAULT_TIMEOUT 5

struct config {
    int port;
    const char *root;
    const char *model;
    int workers;
    int timeout;
    bool log;
};

#endif
