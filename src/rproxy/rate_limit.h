#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include <stddef.h>

/* Decision module of the Reverse Proxy: a token bucket for each key (for
   example a client IP or a JA4). Time is an argument, so tests need no
   clock. Safe to call from many Workers. */

struct rate_limit_options {
    double rate;    /* tokens that come back each second */
    double burst;   /* the most tokens that one key can hold */
    size_t key_max; /* keys in the table; when it is full, the key that
                       was used longest ago is forgotten */
};

/* Returns NULL if memory or the lock cannot be set up. */
struct rate_limit *rate_limit_create(const struct rate_limit_options *options);
void rate_limit_destroy(struct rate_limit *limit);

/* Takes one token of key at time now (seconds, any fixed origin). A new
   key starts with a full bucket. Returns 0 if a token was there, or the
   whole seconds until the next token comes (at least 1), for Retry-After.
   A refused request takes no token. */
int rate_limit_take(struct rate_limit *limit, const char *key, double now);

#endif
