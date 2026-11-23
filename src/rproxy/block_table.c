#include "block_table.h"

#include <stdlib.h>

struct block_table {
    size_t key_max;
};

struct block_table *block_table_create(size_t key_max)
{
    struct block_table *blocks = malloc(sizeof(*blocks));
    if (blocks != NULL) {
        blocks->key_max = key_max;
    }

    return blocks;
}

void block_table_destroy(struct block_table *blocks)
{
    free(blocks);
}

bool block_table_add(struct block_table *blocks, const char *key, double until,
                     double now)
{
    (void)blocks;
    (void)key;
    (void)until;
    (void)now;

    return false;
}

bool block_table_has_key(struct block_table *blocks, const char *key,
                         double now)
{
    (void)blocks;
    (void)key;
    (void)now;

    return false;
}
