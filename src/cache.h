#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "config.h"

/* I/O layer. The Reverse Proxy cache: a key ("GET /path?query") maps to a
   full Origin reply. Safe to call from many threads. */

struct cache_stats {
    unsigned long hits;
    unsigned long misses;
    unsigned long rejects;
    unsigned long evictions;
    unsigned long expirations;
    size_t bytes;
    size_t entries;
};

/* Returns NULL if memory or a lock cannot be set up. */
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

/* True if the reply has an ETag header that if_none_match names. The
   comparison is the weak comparison of RFC 9110 section 13.1.2, and "*"
   names any ETag. A Shared function with no lock: it reads only reply. */
bool cache_is_not_modified(const char *reply, size_t reply_len,
                           const char *if_none_match);
/* Returns the ETag value in the header section of reply, with its quotes
   and no spaces around it, and sets *etag_len. NULL if it has none. */
const char *cache_find_etag(const char *reply, size_t reply_len,
                            size_t *etag_len);

void cache_stats_get(struct cache *cache, struct cache_stats *stats);

/* One line to stderr: "cache pid=... hits=... misses=... ...". */
void cache_report(struct cache *cache);

#endif
