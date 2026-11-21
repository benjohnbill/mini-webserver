#include "route.h"

#include <arpa/inet.h>
#include <string.h>

static int parse_backend(backend *b, const char *start, const char *end)
{
    const char *separator;
    const char *host_end;
    if (start == end)
        return -1;

    int bracketed = *start == '[';
    if (bracketed) {
        ++start;
        host_end = memchr(start, ']', (size_t)(end - start));
        if (!host_end || host_end + 1 == end || host_end[1] != ':')
            return -1;
        separator = host_end + 1;
    } else {
        separator = memchr(start, ':', (size_t)(end - start));
        if (!separator)
            return -1;
        host_end = separator;
    }

    size_t host_len = (size_t)(host_end - start);
    const char *port = separator + 1;
    size_t port_len = (size_t)(end - port);
    if (host_len == 0 || host_len >= sizeof b->host ||
        port_len == 0 || port_len >= sizeof b->port)
        return -1;

    unsigned int number = 0;
    for (const char *p = port; p < end; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
        number = number * 10 + (unsigned int)(*p - '0');
        if (number > 65535)
            return -1;
    }
    if (number == 0)
        return -1;

    memcpy(b->host, start, host_len);
    b->host[host_len] = '\0';
    if (bracketed) {
        struct in6_addr addr;
        if (inet_pton(AF_INET6, b->host, &addr) != 1)
            return -1;
    }
    memcpy(b->port, port, port_len);
    b->port[port_len] = '\0';
    return 0;
}

void route_table_init(route_table *t)
{
    memset(t, 0, sizeof *t);
}

int route_table_add(route_table *t, const char *spec)
{
    if (!spec || spec[0] != '/' || t->count >= ROUTE_MAX_ROUTES)
        return -1;

    const char *equals = strchr(spec, '=');
    route entry = {0};
    if (!equals || (size_t)(equals - spec) >= sizeof entry.prefix)
        return -1;
    memcpy(entry.prefix, spec, (size_t)(equals - spec));

    /* Publish the route only after every backend has been validated. */
    const char *start = equals + 1;
    for (;;) {
        const char *end = start + strcspn(start, ",");
        if (entry.backend_count == ROUTE_MAX_BACKENDS ||
            parse_backend(&entry.backends[entry.backend_count], start, end) < 0)
            return -1;
        ++entry.backend_count;
        if (*end == '\0')
            break;
        start = end + 1;
    }

    t->routes[t->count++] = entry;
    return 0;
}

route *route_match(route_table *t, const char *path)
{
    route *best = NULL;
    size_t best_len = 0;
    size_t path_len = strlen(path);
    for (size_t i = 0; i < t->count; ++i) {
        route *entry = &t->routes[i];
        size_t len = strlen(entry->prefix);
        if (len <= best_len)
            continue;

        /* Match at a path component boundary, including the bare directory
         * for a route written with a trailing slash. */
        int trailing_slash = entry->prefix[len - 1] == '/';
        if ((path_len >= len && strncmp(entry->prefix, path, len) == 0 &&
             (trailing_slash || path[len] == '/' || path[len] == '\0')) ||
            (len > 1 && trailing_slash && path_len == len - 1 &&
             strncmp(entry->prefix, path, path_len) == 0)) {
            best = entry;
            best_len = len;
        }
    }
    return best;
}

const backend *route_next_backend(route *r)
{
    if (!r || r->backend_count == 0)
        return NULL;
    unsigned int next = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);
    return &r->backends[next % r->backend_count];
}
