#include "behavior.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One array and a linear search, the same shape as rate_limit and
   block_table. The distinct targets of a key are a set of
   BEHAVIOR_TARGET_BITS bits: a target sets the bit of its FNV-1a hash.
   This is a Bloom filter with one hash function, as in linear counting
   (Whang 1990). Two targets with the same bit count as one, so the count
   can be low. It is never high. */

#define NOT_FOUND 404
#define TOO_MANY_REQUESTS 429
#define BITS_PER_WORD 64
#define TARGET_WORDS (BEHAVIOR_TARGET_BITS / BITS_PER_WORD)
#define FNV_OFFSET_BASIS 2166136261U
#define FNV_PRIME 16777619U

struct behavior_entry {
    char key[BEHAVIOR_KEY_MAX + 1];
    double window_start;
    double last; /* the time of the last request: the eviction uses it */
    struct behavior_counts counts;
    uint64_t target_bits[TARGET_WORDS];
};

struct behavior {
    struct behavior_options options;
    struct behavior_entry *entries; /* options.key_max slots */
    size_t count; /* slots in use: entries[0] to entries[count - 1] */
    pthread_mutex_t lock;
};

static bool key_is_valid(const char *key);
static struct behavior_entry *entry_find(struct behavior *counters,
                                         const char *key);
static struct behavior_entry *entry_claim(struct behavior *counters,
                                          const char *key);
static bool entry_is_expired(const struct behavior_entry *entry, double window,
                             double now);
static void entry_reset(struct behavior_entry *entry, double now);
static void entry_count_request(struct behavior_entry *entry,
                                const char *target);
static bool target_add(struct behavior_entry *entry, const char *target);
static bool target_has_path(const char *target, const char *path);
static bool target_is_under(const char *target, const char *prefix);

struct behavior *behavior_create(const struct behavior_options *options)
{
    if (options->key_max == 0) {
        return NULL;
    }
    struct behavior *counters = calloc(1, sizeof(*counters));
    if (counters == NULL) {
        return NULL;
    }
    counters->options = *options;
    counters->entries = calloc(options->key_max, sizeof(*counters->entries));
    if (counters->entries == NULL ||
        pthread_mutex_init(&counters->lock, NULL) != 0) {
        free(counters->entries);
        free(counters);
        return NULL;
    }

    return counters;
}

void behavior_destroy(struct behavior *counters)
{
    if (counters == NULL) {
        return;
    }
    pthread_mutex_destroy(&counters->lock);
    free(counters->entries);
    free(counters);
}

bool behavior_add_request(struct behavior *counters, const char *key,
                          const char *target, double now,
                          struct behavior_counts *counts)
{
    if (!key_is_valid(key)) {
        return false;
    }
    pthread_mutex_lock(&counters->lock);
    struct behavior_entry *entry = entry_find(counters, key);
    if (entry == NULL) {
        entry = entry_claim(counters, key);
        entry_reset(entry, now);
    } else if (entry_is_expired(entry, counters->options.window, now)) {
        entry_reset(entry, now);
    }
    entry->last = now;
    entry_count_request(entry, target);
    *counts = entry->counts;
    pthread_mutex_unlock(&counters->lock);

    return true;
}

bool behavior_add_reply(struct behavior *counters, const char *key, int status,
                        double now)
{
    if (!key_is_valid(key)) {
        return false;
    }
    pthread_mutex_lock(&counters->lock);
    struct behavior_entry *entry = entry_find(counters, key);
    bool is_counted = entry != NULL &&
                      !entry_is_expired(entry, counters->options.window, now);
    if (is_counted && status == NOT_FOUND) {
        entry->counts.not_found++;
    }
    if (is_counted && status == TOO_MANY_REQUESTS) {
        entry->counts.slowed++;
    }
    pthread_mutex_unlock(&counters->lock);

    return is_counted;
}

static bool key_is_valid(const char *key)
{
    size_t key_len = strlen(key);

    return key_len > 0 && key_len <= BEHAVIOR_KEY_MAX;
}

static struct behavior_entry *entry_find(struct behavior *counters,
                                         const char *key)
{
    for (size_t i = 0; i < counters->count; i++) {
        if (strcmp(counters->entries[i].key, key) == 0) {
            return &counters->entries[i];
        }
    }

    return NULL;
}

/* A free slot, or else the slot of the key used longest ago. */
static struct behavior_entry *entry_claim(struct behavior *counters,
                                          const char *key)
{
    struct behavior_entry *entry = &counters->entries[0];
    if (counters->count < counters->options.key_max) {
        entry = &counters->entries[counters->count++];
    } else {
        for (size_t i = 1; i < counters->count; i++) {
            if (counters->entries[i].last < entry->last) {
                entry = &counters->entries[i];
            }
        }
    }
    memcpy(entry->key, key, strlen(key) + 1);

    return entry;
}

static bool entry_is_expired(const struct behavior_entry *entry, double window,
                             double now)
{
    return now - entry->window_start >= window;
}

static void entry_reset(struct behavior_entry *entry, double now)
{
    entry->window_start = now;
    entry->counts = (struct behavior_counts){0};
    memset(entry->target_bits, 0, sizeof(entry->target_bits));
}

/* The path of the target (before "?") decides a robots.txt read and a
   trap hit. The whole target, query too, is one distinct target. */
static void entry_count_request(struct behavior_entry *entry,
                                const char *target)
{
    entry->counts.requests++;
    if (target_add(entry, target)) {
        entry->counts.targets++;
    }
    if (target_has_path(target, BEHAVIOR_ROBOTS_PATH)) {
        entry->counts.robots_reads++;
    }
    if (target_is_under(target, BEHAVIOR_TRAP_PREFIX)) {
        entry->counts.trap_hits++;
    }
}

/* Sets the bit of target. Returns true if the bit was not set before. */
static bool target_add(struct behavior_entry *entry, const char *target)
{
    uint32_t hash = FNV_OFFSET_BASIS;
    for (const unsigned char *at = (const unsigned char *)target; *at != '\0';
         at++) {
        hash = (hash ^ *at) * FNV_PRIME;
    }
    uint32_t bit = hash % BEHAVIOR_TARGET_BITS;
    uint64_t mask = (uint64_t)1 << (bit % BITS_PER_WORD);
    uint64_t *word = &entry->target_bits[bit / BITS_PER_WORD];
    bool is_new = (*word & mask) == 0;
    *word |= mask;

    return is_new;
}

static bool target_has_path(const char *target, const char *path)
{
    size_t path_len = strlen(path);

    return strncmp(target, path, path_len) == 0 &&
           (target[path_len] == '\0' || target[path_len] == '?');
}

static bool target_is_under(const char *target, const char *prefix)
{
    return strncmp(target, prefix, strlen(prefix)) == 0;
}
