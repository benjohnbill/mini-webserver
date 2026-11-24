#ifndef FORWARD_H
#define FORWARD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

#include "http_parse.h"

/* I/O layer of the Reverse Proxy: the Origins, and the request for one. */

#define ORIGIN_MAX 8
#define ORIGIN_HOST_MAX 64
#define ORIGIN_PORT_MAX 8
/* getaddrinfo can give more than one address for a name (IPv6 and IPv4). */
#define ORIGIN_ADDRESS_MAX 4

struct origin {
    char host[ORIGIN_HOST_MAX];
    char port[ORIGIN_PORT_MAX];
    /* Set by origin_resolve, once at the start, in the order of
       getaddrinfo. */
    struct sockaddr_storage addresses[ORIGIN_ADDRESS_MAX];
    socklen_t address_lens[ORIGIN_ADDRESS_MAX];
    size_t address_count;
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

/* Resolves the host and the port of origin into up to
   ORIGIN_ADDRESS_MAX addresses, with getaddrinfo. Returns 0, or the
   getaddrinfo error code. */
int origin_resolve(struct origin *origin);

/* Starts a non-blocking connect to address number index of origin.
   Returns the socket, which is non-blocking and can still be in the
   connect, or -1 if the connect failed at once. receive_buffer > 0 sets
   SO_RCVBUF before the connect, so the TCP window starts small; 0 keeps
   the kernel autotuning. */
int origin_connect(const struct origin *origin, size_t index,
                   int receive_buffer);

/* After a connect in progress makes origin_fd writable: true if the
   connect worked. */
bool origin_is_connected(int origin_fd);

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

#endif
