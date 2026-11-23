#ifndef BEHAVIOR_H
#define BEHAVIOR_H

#include <stdbool.h>
#include <stddef.h>

/* Decision module of the Reverse Proxy: the Behavior counters of each key
   (a client IP). Each key has a fixed time window. When the window of a
   key ends, its counters start again from zero. Time is an argument
   (seconds, any fixed origin), so tests need no clock. Safe to call from
   many Workers: one mutex guards the table. */

/* The Honeypot: a target whose path starts with this text. */
#define BEHAVIOR_TRAP_PREFIX "/trap/"
#define BEHAVIOR_ROBOTS_PATH "/robots.txt"
/* An IPv6 address (45 characters) fits. */
#define BEHAVIOR_KEY_MAX 63
/* The distinct targets of a key are bits in a set of this size, so the
   count stops at this number. */
#define BEHAVIOR_TARGET_BITS 256

struct behavior_options {
    double window;  /* seconds that the counters of one key last */
    size_t key_max; /* keys in the table; when it is full, the key that
                       was used longest ago is forgotten */
};

struct behavior_counts {
    unsigned requests;
    unsigned targets;   /* distinct targets, at most BEHAVIOR_TARGET_BITS */
    unsigned not_found; /* 404 replies */
    unsigned robots_reads;
    unsigned trap_hits;
};

/* Returns NULL if key_max is 0, or if memory or the lock cannot be set
   up. */
struct behavior *behavior_create(const struct behavior_options *options);
void behavior_destroy(struct behavior *counters);

/* Adds one request of key for target at now, and writes the counts after
   it to counts. A new key, or a key whose window ended, starts at zero
   with a new window at now. Returns false, and changes nothing, if key is
   empty or longer than BEHAVIOR_KEY_MAX. */
bool behavior_add_request(struct behavior *counters, const char *key,
                          const char *target, double now,
                          struct behavior_counts *counts);

/* Adds the status of the reply to the request that behavior_add_request
   added last for key. Only 404 changes a count. Returns false, and changes
   nothing, if key is not in the table or its window ended at now. */
bool behavior_add_reply(struct behavior *counters, const char *key, int status,
                        double now);

#endif
