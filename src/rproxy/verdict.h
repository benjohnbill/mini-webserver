#ifndef VERDICT_H
#define VERDICT_H

#include <stdbool.h>

#include "behavior.h"

/* Decision module of the Reverse Proxy: the Verdict for one request. It
   runs after the TLS handshake and the request parse, so all signals are
   known. */

enum verdict_kind {
    VERDICT_PASS,
    VERDICT_DENY,   /* 403 */
    VERDICT_SLOW,   /* 429 with Retry-After */
    VERDICT_TARPIT, /* hold the connection, then close it with no reply */
};

struct verdict_signals {
    const char *ja4_label;  /* from the JA4 table: NULL if the JA4 is not
                               there, or if there is no JA4 */
    const char *user_agent; /* NULL if absent */
    const char *target;     /* the request target */
    int retry_after;        /* from rate_limit_take: 0 if within the limit */
    struct behavior_counts behavior; /* of the client IP, with this request */
};

struct verdict {
    enum verdict_kind kind;
    int retry_after;    /* for VERDICT_SLOW */
    bool blocks_client; /* put the client IP in the block table */
};

struct verdict verdict_decide(const struct verdict_signals *signals);

#endif
