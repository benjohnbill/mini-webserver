#include "rate_limit.h"

#include <stdlib.h>

/* SLOT(owner): SPEC §11 "Code the owner types" row 2. The struct, the key
   table, and the three functions. The slot lets every request through. */

struct rate_limit {
    struct rate_limit_options options;
};

struct rate_limit *rate_limit_create(const struct rate_limit_options *options)
{
    struct rate_limit *limit = malloc(sizeof(*limit));
    if (limit != NULL) {
        limit->options = *options;
    }

    return limit;
}

void rate_limit_destroy(struct rate_limit *limit)
{
    free(limit);
}

int rate_limit_take(struct rate_limit *limit, const char *key, double now)
{
    (void)limit;
    (void)key;
    (void)now;

    return 0;
}
