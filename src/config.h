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
#define DRAIN_CHUNK_SIZE 4096
#define DRAIN_READS_MAX 16
#define DRAIN_WAIT_MS 1000
#define PORT_MAX 65535
#define WORKERS_MAX 1024
#define TIMEOUT_MAX 3600
/* 4 MiB, so that a 2 MiB file passes rproxy and the size admission of
   the cache has a reply to reject. */
#define ORIGIN_REPLY_MAX 4194304
#define ORIGIN_READ_CHUNK 16384
#define DELAY_MAX 10000
#define WORK_MAX 10000
#define STATS_PERIOD_MS 200
#define CACHE_BUCKETS 1024
/* A multiple of CACHE_BUCKETS, so a key and its seen slot have the same
   stripe. */
#define CACHE_SEEN_SLOTS 4096
#define CACHE_STRIPES 16
#define CACHE_KEY_MAX (TARGET_MAX + 16)
#define CACHE_BYTES_MAX 1073741824
#define CACHE_TTL_MAX 86400

#define DEFAULT_MODEL "pool"
#define DEFAULT_WORKERS 4
#define DEFAULT_TIMEOUT 5
#define DEFAULT_CACHE_BYTES 67108864
#define DEFAULT_CACHE_OBJECT_MAX 1048576

/* Each first value is the default. */
enum cache_admit { CACHE_ADMIT_ALL, CACHE_ADMIT_SIZE, CACHE_ADMIT_SECOND };
enum cache_evict { CACHE_EVICT_FIFO, CACHE_EVICT_LRU };
enum cache_lock { CACHE_LOCK_GLOBAL, CACHE_LOCK_STRIPED };

struct cache_options {
    bool is_on;
    enum cache_admit admit;
    enum cache_evict evict;
    enum cache_lock lock;
    int bytes;      /* the budget for all entries */
    int object_max; /* admit size: the largest reply */
    int ttl;        /* seconds, 0: no expiry */
};

struct cache;

struct config {
    int port;
    const char *root;
    const char *model;
    int workers;
    int timeout;
    bool log;
    int delay_ms;           /* sleep before each answer: a slow Origin */
    int work_ms;            /* CPU time before each answer: the Handler time */
    const char *stats_path; /* --stats: epoll models write it, or NULL */
};

#endif
