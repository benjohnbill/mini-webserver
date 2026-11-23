#include "block_table.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* One array and a linear search. key_max is a few thousand, and a search
   is cheap next to a TLS handshake. */

struct block_entry {
    char key[BLOCK_KEY_MAX + 1];
    double until;
};

struct block_table {
    struct block_entry *entries;
    size_t count;
    size_t key_max;
    pthread_mutex_t lock;
};

static struct block_entry *entry_find(struct block_table *blocks,
                                      const char *key);
static struct block_entry *entry_take_slot(struct block_table *blocks,
                                           double now);
static void entries_remove_expired(struct block_table *blocks, double now);
static void entry_remove(struct block_table *blocks, struct block_entry *entry);

struct block_table *block_table_create(size_t key_max)
{
    if (key_max == 0) {
        return NULL;
    }
    struct block_table *blocks = calloc(1, sizeof(*blocks));
    if (blocks == NULL) {
        return NULL;
    }
    blocks->key_max = key_max;
    blocks->entries = calloc(key_max, sizeof(*blocks->entries));
    if (blocks->entries == NULL ||
        pthread_mutex_init(&blocks->lock, NULL) != 0) {
        free(blocks->entries);
        free(blocks);
        return NULL;
    }

    return blocks;
}

void block_table_destroy(struct block_table *blocks)
{
    if (blocks == NULL) {
        return;
    }
    pthread_mutex_destroy(&blocks->lock);
    free(blocks->entries);
    free(blocks);
}

bool block_table_add(struct block_table *blocks, const char *key, double until,
                     double now)
{
    size_t key_len = strlen(key);
    if (key_len == 0 || key_len > BLOCK_KEY_MAX) {
        return false;
    }
    pthread_mutex_lock(&blocks->lock);
    struct block_entry *entry = entry_find(blocks, key);
    if (entry == NULL) {
        entry = entry_take_slot(blocks, now);
        memcpy(entry->key, key, key_len + 1);
        entry->until = until;
    } else if (until > entry->until) {
        entry->until = until;
    }
    pthread_mutex_unlock(&blocks->lock);

    return true;
}

bool block_table_has_key(struct block_table *blocks, const char *key,
                         double now)
{
    pthread_mutex_lock(&blocks->lock);
    const struct block_entry *entry = entry_find(blocks, key);
    bool is_blocked = entry != NULL && now < entry->until;
    pthread_mutex_unlock(&blocks->lock);

    return is_blocked;
}

static struct block_entry *entry_find(struct block_table *blocks,
                                      const char *key)
{
    for (size_t i = 0; i < blocks->count; i++) {
        if (strcmp(blocks->entries[i].key, key) == 0) {
            return &blocks->entries[i];
        }
    }

    return NULL;
}

/* Returns a free entry at the end of the array. When the table is full,
   the expired keys leave first. If none expired, the key that expires
   first leaves. */
static struct block_entry *entry_take_slot(struct block_table *blocks,
                                           double now)
{
    if (blocks->count == blocks->key_max) {
        entries_remove_expired(blocks, now);
    }
    if (blocks->count == blocks->key_max) {
        struct block_entry *first = &blocks->entries[0];
        for (size_t i = 1; i < blocks->count; i++) {
            if (blocks->entries[i].until < first->until) {
                first = &blocks->entries[i];
            }
        }
        entry_remove(blocks, first);
    }

    return &blocks->entries[blocks->count++];
}

static void entries_remove_expired(struct block_table *blocks, double now)
{
    size_t index = 0;
    while (index < blocks->count) {
        if (now >= blocks->entries[index].until) {
            entry_remove(blocks, &blocks->entries[index]);
        } else {
            index++;
        }
    }
}

/* Moves the last entry into the hole. */
static void entry_remove(struct block_table *blocks, struct block_entry *entry)
{
    blocks->count--;
    *entry = blocks->entries[blocks->count];
}
