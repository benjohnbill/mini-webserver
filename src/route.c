#include "route.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* FNV-1a alone clusters on similar keys ("host:port-1", "-2"...); the murmur3
 * finalizer spreads them over the whole ring. */
static uint32_t ring_hashstr(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (unsigned char)*s) * 16777619u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static int parse_backend(backend *b, unsigned *weight,
                         const char *start, const char *end)
{
    const char *separator;
    const char *host_end;
    if (start == end)
        return -1;

    *weight = 1;
    const char *star = memchr(start, '*', (size_t)(end - start));
    if (star) {
        if (end - star != 2 || star == start ||
            star[1] < '1' || star[1] > '0' + ROUTE_MAX_WEIGHT)
            return -1;
        *weight = (unsigned)(star[1] - '0');
        end = star;
    }

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
    snprintf(b->port, sizeof b->port, "%hu", (unsigned short)number);
    return 0;
}

typedef struct {
    uint32_t hash;
    uint8_t backend;
} ring_point;

static int ring_point_cmp(const void *a, const void *b)
{
    const ring_point *x = a, *y = b;
    if (x->hash != y->hash)
        return x->hash < y->hash ? -1 : 1;
    return (x->backend > y->backend) - (x->backend < y->backend);
}

static void build_ring(route *r)
{
    ring_point points[ROUTE_RING_MAX];
    size_t n = 0;
    for (size_t i = 0; i < r->backend_count; ++i) {
        const backend *b = r->backends[i];
        for (unsigned k = 0; k < ROUTE_RING_POINTS * r->weight[i]; ++k) {
            char name[300];
            snprintf(name, sizeof name, "%s:%s-%u", b->host, b->port, k);
            points[n].hash = ring_hashstr(name);
            points[n].backend = (uint8_t)i;
            ++n;
        }
    }
    qsort(points, n, sizeof points[0], ring_point_cmp);
    for (size_t j = 0; j < n; ++j) {
        r->ring_hash[j] = points[j].hash;
        r->ring_backend[j] = points[j].backend;
    }
    r->ring_len = n;
}

void route_table_init(route_table *t)
{
    memset(t, 0, sizeof *t);
    t->refs = 1;
}

static void release_backends(route *r)
{
    for (size_t i = 0; i < r->backend_count; ++i) {
        backend *b = r->backends[i];
        if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL) == 0)
            free(b);
    }
}

void route_table_release(route_table *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    for (size_t i = 0; i < t->count; ++i)
        release_backends(&t->routes[i]);
    if (t->heap)
        free(t);
}

void route_live_init(route_live *l, route_table *t)
{
    pthread_mutex_init(&l->lock, NULL);
    l->cur = t;
}

route_table *route_live_acquire(route_live *l)
{
    pthread_mutex_lock(&l->lock);
    route_table *t = l->cur;
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&l->lock);
    return t;
}

void route_live_swap(route_live *l, route_table *t)
{
    pthread_mutex_lock(&l->lock);
    route_table *old = l->cur;
    l->cur = t;
    pthread_mutex_unlock(&l->lock);
    route_table_release(old);
}

void route_live_destroy(route_live *l)
{
    route_table_release(l->cur);
    pthread_mutex_destroy(&l->lock);
}

static backend *find_backend(const route *r, const backend *key)
{
    for (size_t i = 0; i < r->backend_count; ++i) {
        backend *b = r->backends[i];
        if (strcmp(b->host, key->host) == 0 && strcmp(b->port, key->port) == 0)
            return b;
    }
    return NULL;
}

static const char *add_backend(route *r, const route *old,
                               const char *start, const char *end)
{
    size_t i = r->backend_count;
    backend parsed = {0};
    if (i == ROUTE_MAX_BACKENDS)
        return "too many backends";
    if (parse_backend(&parsed, &r->weight[i], start, end) < 0)
        return "invalid backend token";
    if (r->file[0] && find_backend(r, &parsed))
        return "duplicate backend";

    /* shortcut: a quick re-add splits inflight across objects while old requests run, use a global backend registry if this matters. */
    backend *b = old ? find_backend(old, &parsed) : NULL;
    if (b) {
        __atomic_add_fetch(&b->refs, 1, __ATOMIC_RELAXED);
    } else {
        b = malloc(sizeof *b);
        if (!b)
            return "out of memory";
        parsed.refs = 1;
        parsed.changed_ms = now_ms();
        *b = parsed;
    }
    r->backends[r->backend_count++] = b;
    return NULL;
}

static const char *read_route_file(route *r, const route *old)
{
    enum { FILE_LIMIT = 64 * 1024 };
    char text[FILE_LIMIT + 1];
    /* A FIFO substituted for the route file must not block the accept loop. */
    int fd = open(r->file, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return strerror(errno);
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return "route file is not a regular file";
    }
    size_t len = 0;
    while (len < sizeof text) {
        ssize_t n = read(fd, text + len, sizeof text - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            int error = errno;
            close(fd);
            return strerror(error);
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);
    if (len > FILE_LIMIT)
        return "route file exceeds 64 KiB";
    if (memchr(text, '\0', len))
        return "NUL byte in route file";
    text[len] = '\0';

    const char *separators = ", \t\r\n";
    const char *p = text + strspn(text, separators);
    while (*p) {
        const char *end;
        if (*p == '#') {
            end = p + strcspn(p, "\r\n");
        } else {
            end = p + strcspn(p, ", \t\r\n#");
            const char *error = add_backend(r, old, p, end);
            if (error)
                return error;
        }
        p = end + strspn(end, separators);
    }
    return r->backend_count ? NULL : "empty backend list";
}

int route_table_reload(const route_table *old, route_table **out,
                       char *err, size_t err_size)
{
    route_table *t = malloc(sizeof *t);
    if (!t) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    route_table_init(t);
    t->heap = 1;
    for (size_t i = 0; i < old->count; ++i) {
        const route *prev = &old->routes[i];
        /* Include partial routes in release if loading fails. */
        route *r = &t->routes[t->count++];
        memcpy(r->prefix, prev->prefix, sizeof r->prefix);
        memcpy(r->file, prev->file, sizeof r->file);
        r->ketama = prev->ketama;
        r->next = __atomic_load_n(&prev->next, __ATOMIC_RELAXED);
        if (r->file[0]) {
            const char *error = read_route_file(r, prev);
            if (error) {
                snprintf(err, err_size, "%s: %s", r->file, error);
                for (size_t j = 0; j < err_size && err[j]; ++j)
                    if (err[j] == '\r' || err[j] == '\n')
                        err[j] = ' ';
                route_table_release(t);
                return -1;
            }
        } else {
            for (size_t j = 0; j < prev->backend_count; ++j) {
                backend *b = prev->backends[j];
                __atomic_add_fetch(&b->refs, 1, __ATOMIC_RELAXED);
                r->backends[j] = b;
                r->weight[j] = prev->weight[j];
            }
            r->backend_count = prev->backend_count;
        }
        if (r->ketama)
            build_ring(r);
    }
    *out = t;
    return 0;
}

int route_table_add(route_table *t, const char *spec)
{
    if (!spec || spec[0] != '/' || t->count >= ROUTE_MAX_ROUTES)
        return -1;

    const char *equals = strchr(spec, '=');
    route entry = {0};
    if (!equals)
        return -1;
    static const char tag[] = "@ketama";
    size_t prefix_len = (size_t)(equals - spec);
    if (prefix_len > sizeof tag - 1 &&
        memcmp(equals - (sizeof tag - 1), tag, sizeof tag - 1) == 0) {
        entry.ketama = 1;
        prefix_len -= sizeof tag - 1;
    }
    if (prefix_len >= sizeof entry.prefix)
        return -1;
    memcpy(entry.prefix, spec, prefix_len);

    const char *start = equals + 1;
    if (*start == '@') {
        char *path = realpath(start + 1, NULL);
        if (!path) {
            fprintf(stderr, "route file: %s\n", strerror(errno));
            return -1;
        }
        if (strlen(path) >= sizeof entry.file) {
            free(path);
            fprintf(stderr, "route file: path is too long\n");
            return -1;
        }
        strcpy(entry.file, path);
        free(path);
        const char *error = read_route_file(&entry, NULL);
        if (error) {
            fprintf(stderr, "route file: %s\n", error);
            release_backends(&entry);
            return -1;
        }
    } else {
        for (;;) {
            const char *end = start + strcspn(start, ",");
            if (add_backend(&entry, NULL, start, end)) {
                release_backends(&entry);
                return -1;
            }
            if (*end == '\0')
                break;
            start = end + 1;
        }
    }

    if (entry.ketama)
        build_ring(&entry);
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

/* Weighted rr slot s of the cycle: round k lists every backend with weight > k,
 * so all-weight-1 stays b0,b1,b2 and 2:1:1 becomes b0,b1,b2,b0. */
static size_t rr_slot(const route *r, unsigned s)
{
    for (unsigned k = 0;; ++k)
        for (size_t i = 0; i < r->backend_count; ++i)
            if (r->weight[i] > k && s-- == 0)
                return i;
}

/* Adds backend i to out unless seen; returns 1 when out is full or all seen. */
static int pick_one(route *r, size_t i, unsigned *seen, backend **out,
                    size_t *n, size_t max)
{
    if (!(*seen & (1u << i))) {
        *seen |= 1u << i;
        if (backend_is_up(r->backends[i]))
            out[(*n)++] = r->backends[i];
    }
    return *n == max || *seen == (1u << r->backend_count) - 1;
}

size_t route_pick(route *r, const char *key, backend **out, size_t max)
{
    size_t n = 0;
    unsigned seen = 0;
    if (!r || r->backend_count == 0 || max == 0)
        return 0;

    if (r->ketama) {
        uint32_t h = ring_hashstr(key ? key : "");
        size_t lo = 0, hi = r->ring_len;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (r->ring_hash[mid] < h)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (size_t j = 0; j < r->ring_len; ++j)
            if (pick_one(r, r->ring_backend[(lo + j) % r->ring_len], &seen, out, &n, max))
                break;
        return n;
    }

    unsigned total = 0;
    for (size_t i = 0; i < r->backend_count; ++i)
        total += r->weight[i];
    /* Reduce first so cursor + j cannot wrap past UINT_MAX and repeat a slot. */
    unsigned cursor = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED) % total;
    for (unsigned j = 0; j < total; ++j)
        if (pick_one(r, rr_slot(r, (cursor + j) % total), &seen, out, &n, max))
            break;
    return n;
}

double route_share(const route *r, size_t i)
{
    if (!r || i >= r->backend_count)
        return 0.0;
    if (!r->ketama) {
        unsigned total = 0;
        for (size_t k = 0; k < r->backend_count; ++k)
            total += r->weight[k];
        return (double)r->weight[i] / total;
    }
    /* Keys in (previous point, this point] go to this point's backend. */
    uint64_t owned = 0;
    for (size_t j = 0; j < r->ring_len; ++j)
        if (r->ring_backend[j] == i)
            owned += (uint32_t)(r->ring_hash[j] -
                                r->ring_hash[j ? j - 1 : r->ring_len - 1]);
    return (double)owned / 4294967296.0;
}

void backend_report(backend *b, int ok)
{
    /* CAS on down makes exactly one concurrent reporter perform the flip. */
    int expected = ok ? 1 : 0;           /* ok flips DOWN->UP, fail flips UP->DOWN */
    if (ok) {
        __atomic_store_n(&b->fails, 0, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&b->oks, 1, __ATOMIC_RELAXED) >= HEALTH_OKS_TO_UP &&
            __atomic_compare_exchange_n(&b->down, &expected, 0, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            __atomic_store_n(&b->changed_ms, now_ms(), __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&b->oks, 0, __ATOMIC_RELAXED);
        /* shortcut: a stale fail count racing two oks can still flip DOWN (rare transient flap, self-heals on the next 2 oks); upgrade to a per-backend lock if flapping is observed. */
        if (__atomic_add_fetch(&b->fails, 1, __ATOMIC_RELAXED) >= HEALTH_FAILS_TO_DOWN &&
            __atomic_compare_exchange_n(&b->down, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            __atomic_store_n(&b->changed_ms, now_ms(), __ATOMIC_RELAXED);
    }
}

int backend_is_up(const backend *b)
{
    return !__atomic_load_n(&b->down, __ATOMIC_ACQUIRE);
}

int backend_try_acquire(backend *b, unsigned cap)
{
    /* Fetch-add then rollback would expose counts above the cap to readers. */
    unsigned current = __atomic_load_n(&b->inflight, __ATOMIC_RELAXED);
    while (current < cap) {
        if (__atomic_compare_exchange_n(&b->inflight, &current, current + 1, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

void backend_release(backend *b)
{
    __atomic_fetch_sub(&b->inflight, 1, __ATOMIC_RELAXED);
}
