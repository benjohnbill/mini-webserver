#ifndef BLOCK_TABLE_H
#define BLOCK_TABLE_H

#include <stdbool.h>
#include <stddef.h>

/* Decision module of the Reverse Proxy: the block table. A key (a client
   IP or a JA4) is blocked until a time. Time is an argument (seconds, any
   fixed origin), so tests need no clock. Safe to call from many Workers:
   one mutex guards the table. */

/* An IPv6 address (45 characters) and a JA4 (36) fit. */
#define BLOCK_KEY_MAX 63

/* Returns NULL if key_max is 0, or if memory or the lock cannot be set
   up. */
struct block_table *block_table_create(size_t key_max);
void block_table_destroy(struct block_table *blocks);

/* Blocks key until the time until. If key is there, it keeps the later of
   the two times. When the table holds key_max keys, the keys that expired
   at now leave first. If none expired, the key that expires first leaves.
   Returns false, and changes nothing, if key is empty or longer than
   BLOCK_KEY_MAX. */
bool block_table_add(struct block_table *blocks, const char *key, double until,
                     double now);

/* Asks if key is blocked at now. A key is blocked while now < until. */
bool block_table_has_key(struct block_table *blocks, const char *key,
                         double now);

#endif
