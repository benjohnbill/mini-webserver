#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

#include "config.h"
#include "http_parse.h"

/* I/O layer. The full upstream reply: status line, headers, and body. */
struct proxy_reply {
    char *data;
    size_t len;
};

/* Sends request to the upstream of config, with PROXY_PREFIX taken off the
   target, and reads the reply to EOF. Returns 0, 502 (connect, read, or
   size error), or 504 (the upstream timed out). */
int proxy_fetch(const struct request *request, const struct config *config,
                struct proxy_reply *reply);
void proxy_reply_destroy(struct proxy_reply *reply);

#endif
