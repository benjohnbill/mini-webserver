#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "config.h"

/* I/O layer. The Reverse Proxy cache: a key ("GET /path?query") maps to a
   full upstream reply. Safe to call from many threads, and, with
   CACHE_SHARE_SHARED, from many processes after fork. */

struct cache_stats {
    unsigned long hits;
    unsigned long misses;
    unsigned long rejects;
    unsigned long evictions;
    unsigned long expirations;
    size_t bytes;
    size_t entries;
};

/* Returns NULL if memory or a lock cannot be set up. With
   CACHE_SHARE_SHARED, call it before fork. */
struct cache *cache_create(const struct cache_options *options);
void cache_destroy(struct cache *cache);

/* On a hit that is fresh at now, sets *reply to a malloc copy that the
   caller frees, and returns true. */
bool cache_find(struct cache *cache, const char *key, time_t now, char **reply,
                size_t *reply_len);

/* Stores a copy of reply if the admission policy and the budget accept it.
   Only a GET with status 200 is stored. Returns true if it stored. */
bool cache_store(struct cache *cache, const char *key, const char *reply,
                 size_t reply_len, time_t now);

void cache_stats_get(struct cache *cache, struct cache_stats *stats);

/* One line to stderr: "cache pid=... hits=... misses=... ...". */
void cache_report(struct cache *cache);

#endif
