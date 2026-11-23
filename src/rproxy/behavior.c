#include "behavior.h"

#include <stdlib.h>

/* Slot: the tests come first and fail. */

struct behavior {
    struct behavior_options options;
};

struct behavior *behavior_create(const struct behavior_options *options)
{
    struct behavior *counters = calloc(1, sizeof(*counters));
    if (counters != NULL) {
        counters->options = *options;
    }

    return counters;
}

void behavior_destroy(struct behavior *counters)
{
    free(counters);
}

bool behavior_add_request(struct behavior *counters, const char *key,
                          const char *target, double now,
                          struct behavior_counts *counts)
{
    (void)counters;
    (void)key;
    (void)target;
    (void)now;
    *counts = (struct behavior_counts){0};

    return false;
}

bool behavior_add_reply(struct behavior *counters, const char *key, int status,
                        double now)
{
    (void)counters;
    (void)key;
    (void)status;
    (void)now;

    return false;
}
