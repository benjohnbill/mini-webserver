#ifndef FORWARD_H
#define FORWARD_H

#include <stdatomic.h>
#include <stddef.h>

#include "http_parse.h"

/* I/O layer of the Reverse Proxy: one request to an Origin. */

#define ORIGIN_MAX 8
#define ORIGIN_HOST_MAX 64
#define ORIGIN_PORT_MAX 8

struct origin {
    char host[ORIGIN_HOST_MAX];
    char port[ORIGIN_PORT_MAX];
};

/* The Origins of --origin, in the order of the command line. next counts
   the picks, so that the Origins get the requests in turn. */
struct origin_list {
    struct origin origins[ORIGIN_MAX];
    size_t count;
    atomic_size_t next;
};

/* Parses "HOST:PORT" into origin. Returns 0, or -1 if a part is empty,
   too long, or the port is not a number from 1 to 65535. */
int origin_parse(const char *text, struct origin *origin);

/* Returns the index of the Origin for the next request (Round-Robin).
   Safe to call from many Workers. list->count must be at least 1. */
size_t origin_list_pick(struct origin_list *list);

/* Writes the request for an Origin into buffer: the request line, Host,
   User-Agent, Content-Type, Content-Length (the fields that are not NULL
   or -1), X-Forwarded-For, X-JA4, Connection: close, the empty line, and
   the body. Returns the length, or 0 if it does not fit in size bytes. */
size_t forward_format_request(const struct request *request,
                              const char *client_ip, const char *ja4,
                              char *buffer, size_t size);

/* Sends request to the Origin at index first, or to the next Origin if
   the connect fails. Reads the reply to EOF into a malloc buffer (*reply,
   *reply_len) that the caller frees. Returns 0, 502 (no Origin accepted,
   or the reply is too large), or 504 (no reply in timeout seconds). */
int forward_fetch(const struct origin_list *list, size_t first,
                  const char *request_bytes, size_t request_len, int timeout,
                  char **reply, size_t *reply_len);

#endif
