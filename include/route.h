#ifndef ROUTE_H
#define ROUTE_H

#include <stddef.h>

#define ROUTE_MAX_ROUTES   16
#define ROUTE_MAX_BACKENDS 8

typedef struct {
    char host[256];
    char port[8];
} backend;

typedef struct {
    char prefix[256];                     /* e.g. "/api/" */
    backend backends[ROUTE_MAX_BACKENDS];
    size_t backend_count;
    unsigned int next;                    /* round-robin cursor, use atomics */
} route;

typedef struct {
    route routes[ROUTE_MAX_ROUTES];
    size_t count;
} route_table;

void route_table_init(route_table *t);

/* Add a route from a spec string:
 *   "/api/=127.0.0.1:8001,127.0.0.1:8003"
 * prefix must start with '/'. Returns 0 on success, -1 on malformed spec
 * or table full. */
int route_table_add(route_table *t, const char *spec);

/* Longest-prefix match on a decoded request path. NULL if none. */
route *route_match(route_table *t, const char *path);

/* Thread-safe round-robin pick. */
const backend *route_next_backend(route *r);

#endif
