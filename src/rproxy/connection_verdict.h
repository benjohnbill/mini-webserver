#ifndef CONNECTION_VERDICT_H
#define CONNECTION_VERDICT_H

#include <stdbool.h>

/* Decision module of the Reverse Proxy: the connection Verdict. It runs
   after the ClientHello peek and the JA4, before the TLS handshake. There
   is no HTTP yet, so it can only let the connection through, close it, or
   hold it. */

enum connection_verdict {
    CONN_PASS,
    CONN_DROP,   /* close with no reply: no TLS handshake */
    CONN_TARPIT, /* hold the connection, then close it with no reply */
};

struct connection_signals {
    bool is_listed;        /* the client IP is in the block list */
    bool is_blocked;       /* the client IP or the JA4 is in the block table */
    const char *ja4_label; /* from the JA4 table: NULL if not there */
};

enum connection_verdict
connection_decide(const struct connection_signals *signals);

#endif
