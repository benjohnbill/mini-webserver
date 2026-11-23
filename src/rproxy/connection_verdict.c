#include "connection_verdict.h"

enum connection_verdict
connection_decide(const struct connection_signals *signals)
{
    (void)signals;

    return CONN_PASS;
}
