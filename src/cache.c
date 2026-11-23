#include "cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define CACHE_CONTROL_LEN (sizeof("Cache-Control:") - 1)
#define WEAK_PREFIX "W/"
#define WEAK_PREFIX_LEN (sizeof(WEAK_PREFIX) - 1)
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

/* One block holds the entry, then the key, then the reply. */
struct cache_entry {
    char *key;
    char *reply;
    size_t reply_len;
    size_t block_size; /* bytes counted against the budget */
    uint64_t hash;
    time_t stored_at;
    unsigned hits;
    struct cache_entry *prev;  /* order list: toward the oldest */
    struct cache_entry *next;  /* order list: toward the newest */
    struct cache_entry *chain; /* bucket chain */
};

/* A stripe lock guards the chains of its buckets, their seen slots, and
   the counters of the stripe. Global mode has one stripe. */
struct cache_stripe {
    pthread_mutex_t lock;
    unsigned long hits;
    unsigned long misses;
    unsigned long rejects;
};

/* Lock order: a stripe lock, then the order lock. The order lock guards
   the order list, bytes, entries, evictions, and expirations.
   Striped mode: a hit takes only its stripe lock with FIFO. With LRU, a
   hit also takes the order lock to move the entry to the newest end, so
   LRU hits still meet on one lock; striping helps LRU less than FIFO. */
struct cache {
    struct cache_options options;
    int stripe_count;
    struct cache_stripe stripes[CACHE_STRIPES];
    struct cache_entry *buckets[CACHE_BUCKETS];
    uint64_t seen[CACHE_SEEN_SLOTS]; /* admit second: hashes of missed keys */
    pthread_mutex_t order_lock;
    struct cache_entry *oldest;
    struct cache_entry *newest;
    size_t bytes;
    size_t entries;
    unsigned long evictions;
    unsigned long expirations;
};

static bool cache_locks_init(struct cache *cache);
static uint64_t key_hash(const char *key);
static struct cache_stripe *stripe_find(struct cache *cache, uint64_t hash);
static struct cache_entry *bucket_find(struct cache *cache, uint64_t hash,
                                       const char *key);
static bool entry_is_expired(const struct cache *cache,
                             const struct cache_entry *entry, time_t now);
static bool entry_copy(const struct cache_entry *entry, char **reply,
                       size_t *reply_len);
static void entry_touch(struct cache *cache, struct cache_entry *entry);
static void entry_remove(struct cache *cache, struct cache_entry *entry);
static bool reply_is_ok(const char *key, const char *reply, size_t reply_len);
static bool reply_forbids_store(const char *reply, size_t reply_len);
static bool line_has_word(const char *line, size_t line_len, const char *word);
static bool admission_pass(struct cache *cache, uint64_t hash, size_t reply_len,
                           size_t block_size);
static struct cache_entry *entry_create(struct cache *cache, uint64_t hash,
                                        size_t block_size);
static void order_make_room(struct cache *cache, uint64_t hash,
                            size_t block_size);
static void order_append(struct cache *cache, struct cache_entry *entry);
static void order_unlink(struct cache *cache, struct cache_entry *entry);
static const char *reply_find_header(const char *reply, size_t reply_len,
                                     const char *name, size_t *value_len);
static const char *text_trim_spaces(const char *text, size_t *text_len);
static bool char_is_space(char byte);
static bool etag_list_has(const char *list, const char *etag, size_t etag_len);
static const char *etag_strip_weak(const char *etag, size_t *etag_len);

struct cache *cache_create(const struct cache_options *options)
{
    struct cache *cache = calloc(1, sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    cache->options = *options;
    cache->stripe_count =
        options->lock == CACHE_LOCK_STRIPED ? CACHE_STRIPES : 1;
    if (!cache_locks_init(cache)) {
        free(cache);
        return NULL;
    }

    return cache;
}

void cache_destroy(struct cache *cache)
{
    if (cache == NULL) {
        return;
    }
    while (cache->oldest != NULL) {
        entry_remove(cache, cache->oldest);
    }
    for (int i = 0; i < cache->stripe_count; i++) {
        pthread_mutex_destroy(&cache->stripes[i].lock);
    }
    pthread_mutex_destroy(&cache->order_lock);
    free(cache);
}

bool cache_find(struct cache *cache, const char *key, time_t now, char **reply,
                size_t *reply_len)
{
    uint64_t hash = key_hash(key);
    struct cache_stripe *stripe = stripe_find(cache, hash);
    pthread_mutex_lock(&stripe->lock);
    struct cache_entry *entry = bucket_find(cache, hash, key);
    if (entry != NULL && entry_is_expired(cache, entry, now)) {
        pthread_mutex_lock(&cache->order_lock);
        entry_remove(cache, entry);
        cache->expirations++;
        pthread_mutex_unlock(&cache->order_lock);
        entry = NULL;
    }
    /* The copy happens under the stripe lock, so an eviction cannot free
       the entry during the copy. */
    bool is_hit = entry != NULL && entry_copy(entry, reply, reply_len);
    if (is_hit) {
        stripe->hits++;
        entry->hits++;
        entry_touch(cache, entry);
    } else {
        stripe->misses++;
    }
    pthread_mutex_unlock(&stripe->lock);

    return is_hit;
}

bool cache_store(struct cache *cache, const char *key, const char *reply,
                 size_t reply_len, time_t now)
{
    uint64_t hash = key_hash(key);
    size_t key_size = strlen(key) + 1;
    size_t block_size = sizeof(struct cache_entry) + key_size + reply_len;
    struct cache_stripe *stripe = stripe_find(cache, hash);
    pthread_mutex_lock(&stripe->lock);
    struct cache_entry *entry = NULL;
    if (reply_is_ok(key, reply, reply_len) &&
        admission_pass(cache, hash, reply_len, block_size) &&
        bucket_find(cache, hash, key) == NULL) {
        entry = entry_create(cache, hash, block_size);
    }
    if (entry != NULL) {
        entry->key = (char *)(entry + 1);
        entry->reply = entry->key + key_size;
        memcpy(entry->key, key, key_size);
        memcpy(entry->reply, reply, reply_len);
        entry->reply_len = reply_len;
        entry->stored_at = now;
        entry->chain = cache->buckets[hash % CACHE_BUCKETS];
        cache->buckets[hash % CACHE_BUCKETS] = entry;
    } else {
        stripe->rejects++;
    }
    pthread_mutex_unlock(&stripe->lock);

    return entry != NULL;
}

bool cache_is_not_modified(const char *reply, size_t reply_len,
                           const char *if_none_match)
{
    size_t etag_len = 0;
    const char *etag = cache_find_etag(reply, reply_len, &etag_len);
    if (etag == NULL) {
        return false;
    }
    size_t list_len = strlen(if_none_match);
    const char *list = text_trim_spaces(if_none_match, &list_len);
    if (list_len == 1 && list[0] == '*') {
        return true;
    }

    return etag_list_has(if_none_match, etag, etag_len);
}

const char *cache_find_etag(const char *reply, size_t reply_len,
                            size_t *etag_len)
{
    return reply_find_header(reply, reply_len, "ETag", etag_len);
}

void cache_stats_get(struct cache *cache, struct cache_stats *stats)
{
    *stats = (struct cache_stats){0};
    for (int i = 0; i < cache->stripe_count; i++) {
        struct cache_stripe *stripe = &cache->stripes[i];
        pthread_mutex_lock(&stripe->lock);
        stats->hits += stripe->hits;
        stats->misses += stripe->misses;
        stats->rejects += stripe->rejects;
        pthread_mutex_unlock(&stripe->lock);
    }
    pthread_mutex_lock(&cache->order_lock);
    stats->evictions = cache->evictions;
    stats->expirations = cache->expirations;
    stats->bytes = cache->bytes;
    stats->entries = cache->entries;
    pthread_mutex_unlock(&cache->order_lock);
}

void cache_report(struct cache *cache)
{
    struct cache_stats stats;
    cache_stats_get(cache, &stats);
    fprintf(stderr,
            "cache pid=%ld hits=%lu misses=%lu rejects=%lu evictions=%lu "
            "expired=%lu bytes=%zu entries=%zu\n",
            (long)getpid(), stats.hits, stats.misses, stats.rejects,
            stats.evictions, stats.expirations, stats.bytes, stats.entries);
}

static bool cache_locks_init(struct cache *cache)
{
    bool is_ready = pthread_mutex_init(&cache->order_lock, NULL) == 0;
    for (int i = 0; is_ready && i < cache->stripe_count; i++) {
        is_ready = pthread_mutex_init(&cache->stripes[i].lock, NULL) == 0;
    }

    return is_ready;
}

/* FNV-1a, 64 bits. */
static uint64_t key_hash(const char *key)
{
    uint64_t hash = FNV_OFFSET;
    for (const unsigned char *byte = (const unsigned char *)key; *byte != '\0';
         byte++) {
        hash = (hash ^ *byte) * FNV_PRIME;
    }

    return hash;
}

static struct cache_stripe *stripe_find(struct cache *cache, uint64_t hash)
{
    return &cache->stripes[(hash % CACHE_BUCKETS) %
                           (uint64_t)cache->stripe_count];
}

static struct cache_entry *bucket_find(struct cache *cache, uint64_t hash,
                                       const char *key)
{
    struct cache_entry *entry = cache->buckets[hash % CACHE_BUCKETS];
    while (entry != NULL &&
           (entry->hash != hash || strcmp(entry->key, key) != 0)) {
        entry = entry->chain;
    }

    return entry;
}

static bool entry_is_expired(const struct cache *cache,
                             const struct cache_entry *entry, time_t now)
{
    return cache->options.ttl > 0 &&
           now - entry->stored_at >= cache->options.ttl;
}

static bool entry_copy(const struct cache_entry *entry, char **reply,
                       size_t *reply_len)
{
    *reply = malloc(entry->reply_len);
    if (*reply == NULL) {
        return false;
    }
    memcpy(*reply, entry->reply, entry->reply_len);
    *reply_len = entry->reply_len;

    return true;
}

/* The eviction policy: what a hit does to the order list. */
static void entry_touch(struct cache *cache, struct cache_entry *entry)
{
    switch (cache->options.evict) {
    case CACHE_EVICT_LRU:
        pthread_mutex_lock(&cache->order_lock);
        order_unlink(cache, entry);
        order_append(cache, entry);
        pthread_mutex_unlock(&cache->order_lock);
        break;
    case CACHE_EVICT_FIFO:
        break;
    }
}

/* The caller holds the stripe lock of entry and the order lock. */
static void entry_remove(struct cache *cache, struct cache_entry *entry)
{
    struct cache_entry **link = &cache->buckets[entry->hash % CACHE_BUCKETS];
    while (*link != entry) {
        link = &(*link)->chain;
    }
    *link = entry->chain;
    order_unlink(cache, entry);
    cache->bytes -= entry->block_size;
    cache->entries--;
    free(entry);
}

/* Only a GET with status 200 is stored, and only if the Origin allows it
   (RFC 9111 sections 5.2.2.5 and 5.2.2.7). */
static bool reply_is_ok(const char *key, const char *reply, size_t reply_len)
{
    return strncmp(key, "GET ", strlen("GET ")) == 0 &&
           reply_len > strlen("HTTP/1.x 200") &&
           strncmp(reply, "HTTP/1.", strlen("HTTP/1.")) == 0 &&
           strncmp(reply + strlen("HTTP/1.x"), " 200", strlen(" 200")) == 0 &&
           !reply_forbids_store(reply, reply_len);
}

/* True if a Cache-Control line of the header says no-store or private. */
static bool reply_forbids_store(const char *reply, size_t reply_len)
{
    const char *line = reply;
    const char *end = reply + reply_len;
    while (line < end) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        size_t line_len =
            next != NULL ? (size_t)(next - line) : (size_t)(end - line);
        if (line_len <= 1) {
            return false; /* the empty line: the header ends */
        }
        if (line_len > CACHE_CONTROL_LEN &&
            strncasecmp(line, "Cache-Control:", CACHE_CONTROL_LEN) == 0 &&
            (line_has_word(line, line_len, "no-store") ||
             line_has_word(line, line_len, "private"))) {
            return true;
        }
        line = next != NULL ? next + 1 : end;
    }

    return false;
}

static bool line_has_word(const char *line, size_t line_len, const char *word)
{
    size_t word_len = strlen(word);
    for (size_t i = 0; i + word_len <= line_len; i++) {
        if (strncasecmp(line + i, word, word_len) == 0) {
            return true;
        }
    }

    return false;
}

/* The admission policy. For CACHE_ADMIT_SECOND, a first miss records the
   key in the seen set and is rejected. */
static bool admission_pass(struct cache *cache, uint64_t hash, size_t reply_len,
                           size_t block_size)
{
    if (block_size > (size_t)cache->options.bytes) {
        return false;
    }
    uint64_t *seen = &cache->seen[hash % CACHE_SEEN_SLOTS];
    switch (cache->options.admit) {
    case CACHE_ADMIT_SIZE:
        return reply_len <= (size_t)cache->options.object_max;
    case CACHE_ADMIT_SECOND:
        if (*seen == hash) {
            return true;
        }
        *seen = hash;
        return false;
    case CACHE_ADMIT_ALL:
        break;
    }

    return true;
}

/* Evicts to make room, then allocates. The new entry is in the order list
   but not in its chain yet. The caller holds the stripe lock of hash, so no
   other thread evicts it before the caller fills it. */
static struct cache_entry *entry_create(struct cache *cache, uint64_t hash,
                                        size_t block_size)
{
    pthread_mutex_lock(&cache->order_lock);
    order_make_room(cache, hash, block_size);
    struct cache_entry *entry = NULL;
    if (cache->bytes + block_size <= (size_t)cache->options.bytes) {
        entry = malloc(block_size);
    }
    if (entry != NULL) {
        *entry = (struct cache_entry){.hash = hash, .block_size = block_size};
        order_append(cache, entry);
        cache->bytes += block_size;
        cache->entries++;
    }
    pthread_mutex_unlock(&cache->order_lock);

    return entry;
}

/* Evicts from the oldest end. A victim in another stripe is evicted only if
   its stripe lock is free (trylock): waiting for it would break the lock
   order and can deadlock. A busy victim is skipped, so a store can fail to
   make room and be rejected. */
static void order_make_room(struct cache *cache, uint64_t hash,
                            size_t block_size)
{
    struct cache_stripe *own = stripe_find(cache, hash);
    struct cache_entry *victim = cache->oldest;
    while (victim != NULL &&
           cache->bytes + block_size > (size_t)cache->options.bytes) {
        struct cache_entry *next = victim->next;
        struct cache_stripe *stripe = stripe_find(cache, victim->hash);
        if (stripe == own) {
            entry_remove(cache, victim);
            cache->evictions++;
        } else if (pthread_mutex_trylock(&stripe->lock) == 0) {
            entry_remove(cache, victim);
            cache->evictions++;
            pthread_mutex_unlock(&stripe->lock);
        }
        victim = next;
    }
}

static void order_append(struct cache *cache, struct cache_entry *entry)
{
    entry->prev = cache->newest;
    entry->next = NULL;
    if (cache->newest != NULL) {
        cache->newest->next = entry;
    } else {
        cache->oldest = entry;
    }
    cache->newest = entry;
}

static void order_unlink(struct cache *cache, struct cache_entry *entry)
{
    if (entry->prev != NULL) {
        entry->prev->next = entry->next;
    } else {
        cache->oldest = entry->next;
    }
    if (entry->next != NULL) {
        entry->next->prev = entry->prev;
    } else {
        cache->newest = entry->prev;
    }
}

/* Returns the value of the first header line with this name, with no
   spaces around it, or NULL. It reads only the header section. */
static const char *reply_find_header(const char *reply, size_t reply_len,
                                     const char *name, size_t *value_len)
{
    size_t name_len = strlen(name);
    const char *end = reply + reply_len;
    const char *status_end = memchr(reply, '\n', reply_len);
    const char *line = status_end != NULL ? status_end + 1 : end;
    while (line < end) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        size_t line_len =
            next != NULL ? (size_t)(next - line) : (size_t)(end - line);
        if (line_len <= 1) {
            return NULL; /* the empty line: the header ends */
        }
        if (line_len > name_len && line[name_len] == ':' &&
            strncasecmp(line, name, name_len) == 0) {
            *value_len = line_len - name_len - 1;
            return text_trim_spaces(line + name_len + 1, value_len);
        }
        line = next != NULL ? next + 1 : end;
    }

    return NULL;
}

/* Trims spaces, tabs, and a CR from both ends of text_len bytes. */
static const char *text_trim_spaces(const char *text, size_t *text_len)
{
    while (*text_len > 0 && char_is_space(text[0])) {
        text++;
        (*text_len)--;
    }
    while (*text_len > 0 && char_is_space(text[*text_len - 1])) {
        (*text_len)--;
    }

    return text;
}

static bool char_is_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r';
}

/* The weak comparison of RFC 9110 section 13.1.2: both sides lose a W/
   prefix, and the opaque tags compare with their quotes. */
static bool etag_list_has(const char *list, const char *etag, size_t etag_len)
{
    etag = etag_strip_weak(etag, &etag_len);
    while (*list != '\0') {
        size_t item_len = strcspn(list, ",");
        const char *next = list + item_len;
        const char *item =
            etag_strip_weak(text_trim_spaces(list, &item_len), &item_len);
        if (item_len == etag_len && memcmp(item, etag, etag_len) == 0) {
            return true;
        }
        list = *next == ',' ? next + 1 : next;
    }

    return false;
}

static const char *etag_strip_weak(const char *etag, size_t *etag_len)
{
    if (*etag_len >= WEAK_PREFIX_LEN &&
        memcmp(etag, WEAK_PREFIX, WEAK_PREFIX_LEN) == 0) {
        *etag_len -= WEAK_PREFIX_LEN;
        return etag + WEAK_PREFIX_LEN;
    }

    return etag;
}
