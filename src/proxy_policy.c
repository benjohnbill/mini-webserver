#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "proxy_policy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { CACHE_SLOTS = 64, RATE_SLOTS = 1024, MAX_TTL = 60 };

typedef struct {
    CacheTicket ticket;
    uint64_t last_used;
    size_t header_len, body_len;
    char header[CACHE_HEADER_SIZE];
    unsigned char body[RELAY_CHUNK];
} CacheEntry;

typedef struct {
    struct in_addr ip;
    int64_t last_ms;
    unsigned credit; /* thousandths of a request */
    bool used;
} Bucket;

/* CJG's bounded cache and client-IP token bucket share state here across
   workers, event-loop connections and forked children. No process-local pointers. */
struct ProxyPolicy {
    pthread_mutex_t lock;
    uint64_t generation, accesses;
    CacheEntry cache[CACHE_SLOTS];
    Bucket buckets[RATE_SLOTS];
};

ProxyPolicy *proxy_policy_new(void)
{
    ProxyPolicy *p = mmap(NULL, sizeof *p, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
        int error = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (!error)
            error = pthread_mutex_init(&p->lock, &attr);
        pthread_mutexattr_destroy(&attr);
        if (!error)
            return p;
    }
    munmap(p, sizeof *p);
    return NULL;
}

void proxy_policy_free(ProxyPolicy *p)
{
    if (p) {
        pthread_mutex_destroy(&p->lock);
        munmap(p, sizeof *p);
    }
}

bool proxy_rate_allow(ProxyPolicy *p, struct in_addr ip, uint16_t rate, uint16_t burst)
{
    unsigned capacity = (unsigned)burst * 1000;
    pthread_mutex_lock(&p->lock);
    int64_t now = net_now_ms();
    Bucket *b = NULL;
    /* Only replace a bucket once it has fully refilled. A full table cannot
       reset someone else's depleted allowance. */
    for (size_t i = 0; i < RATE_SLOTS; i++) {
        Bucket *item = &p->buckets[i];
        if (item->used && item->ip.s_addr == ip.s_addr) {
            b = item;
            break;
        }
        if (!b && (!item->used || (now - item->last_ms) * rate + item->credit >= capacity))
            b = item;
        if (!item->used)
            break;
    }
    bool allowed = false;
    if (b) {
        if (!b->used || b->ip.s_addr != ip.s_addr)
            *b = (Bucket){.ip = ip, .last_ms = now, .credit = capacity, .used = true};
        int64_t credit = b->credit + (now - b->last_ms) * rate;
        b->credit = credit < capacity ? (unsigned)credit : capacity;
        b->last_ms = now;
        allowed = b->credit >= 1000;
        if (allowed)
            b->credit -= 1000;
    }
    pthread_mutex_unlock(&p->lock);
    return allowed;
}

bool proxy_cache_key(CacheTicket *t, const Request *req, const Backend *backend)
{
    if (strcmp(req->method, "GET") || req->head.length)
        return false;
    const char *host = "";
    for (size_t i = 0; i < req->head.count; i++) {
        Header h = req->head.headers[i];
        if (!strcasecmp(h.name, "Host"))
            host = h.value;
        else if (strcasecmp(h.name, "Accept") && strcasecmp(h.name, "Accept-Encoding") &&
                 strcasecmp(h.name, "Accept-Language") && strcasecmp(h.name, "User-Agent") &&
                 strcasecmp(h.name, "Connection") && strcasecmp(h.name, "Content-Length"))
            return false;
    }
    int n = snprintf(t->key, sizeof t->key, "%s:%u %s %s", backend->ip,
                     (unsigned)backend->port, host, req->target);
    if (n >= 0 && (size_t)n < sizeof t->key)
        return true;
    t->key[0] = 0;
    return false;
}

bool proxy_cache_get(ProxyPolicy *p, CacheTicket *t, HttpOutput *header,
                     unsigned char body[RELAY_CHUNK], size_t *length)
{
    pthread_mutex_lock(&p->lock);
    t->generation = p->generation;
    int64_t now = net_now_ms();
    bool hit = false;
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        CacheEntry *e = &p->cache[i];
        if (e->ticket.generation != p->generation || e->ticket.expires_ms <= now ||
            strcmp(e->ticket.key, t->key))
            continue;
        e->last_used = ++p->accesses;
        header->length = e->header_len - 2; /* replace the final empty line */
        memcpy(header->data, e->header, header->length);
        unsigned age = e->ticket.initial_age + (unsigned)((now - e->ticket.stored_ms) / 1000);
        if (http_append(header, "Age: %u\r\n\r\n", age) == 0) {
            *length = e->body_len;
            memcpy(body, e->body, *length);
            hit = true;
        }
        break;
    }
    pthread_mutex_unlock(&p->lock);
    return hit;
}

/* Accept only explicit shared freshness. Unknown directives and variants
   bypass this small cache and are forwarded unchanged. */
static bool cache_directives(const char *value, bool *is_public, int *ttl)
{
    while (*value) {
        value += strspn(value, " \t,");
        size_t n = strcspn(value, ",");
        const char *next = value + n;
        while (n && (value[n-1] == ' ' || value[n-1] == '\t'))
            n--;
        if (n == 6 && !strncasecmp(value, "public", n)) {
            *is_public = true;
        } else if (n > 8 && !strncasecmp(value, "max-age=", 8) && *ttl < 0) {
            int seconds = 0;
            for (size_t j = 8; j < n; j++) {
                if (value[j] < '0' || value[j] > '9')
                    return false;
                if (seconds < MAX_TTL)
                    seconds = seconds * 10 + value[j] - '0';
            }
            *ttl = seconds < MAX_TTL ? seconds : MAX_TTL;
        } else {
            return false;
        }
        value = next;
    }
    return true;
}

bool proxy_cache_admit(CacheTicket *t, const UpstreamResponse *response, int64_t started_ms)
{
    const HttpHead *h = &response->head;
    if (!t->key[0] || response->status != 200 || !h->has_length || h->length > RELAY_CHUNK)
        return false;
    int ttl = -1;
    bool is_public = false;
    double date_age = 0;
    for (size_t i = 0; i < h->count; i++) {
        Header field = h->headers[i];
        if (!strcasecmp(field.name, "Cache-Control")) {
            if (!cache_directives(field.value, &is_public, &ttl))
                return false;
        } else if (!strcasecmp(field.name, "Date")) {
            struct tm date = {0};
            char *end = strptime(field.value, "%a, %d %b %Y %H:%M:%S GMT", &date);
            if (!end || *end)
                return false;
            double age = difftime(time(NULL), timegm(&date));
            if (age > date_age)
                date_age = age;
        } else if (!strcasecmp(field.name, "Vary") || !strcasecmp(field.name, "Set-Cookie") ||
                   !strcasecmp(field.name, "Age") || !strcasecmp(field.name, "Content-Range") ||
                   !strcasecmp(field.name, "Trailer") || !strcasecmp(field.name, "Pragma")) {
            return false;
        }
    }
    int64_t now = net_now_ms();
    int64_t age_ms = now - started_ms;
    if (!is_public || ttl <= 0 || !http_forward_header(h, "Cache-Control") ||
        date_age >= ttl || age_ms >= ttl * 1000)
        return false;
    if (date_age * 1000 > age_ms)
        age_ms = (int64_t)date_age * 1000;
    t->stored_ms = now;
    t->initial_age = (unsigned)(age_ms / 1000);
    t->expires_ms = now + ttl * 1000 - age_ms;
    return true;
}

void proxy_cache_put(ProxyPolicy *p, const CacheTicket *t, const HttpOutput *header,
                     const unsigned char *body, size_t length)
{
    if (header->length < 2 || header->length > CACHE_HEADER_SIZE || length > RELAY_CHUNK)
        return;
    pthread_mutex_lock(&p->lock);
    int64_t now = net_now_ms();
    if (t->generation == p->generation && t->expires_ms > now) {
        CacheEntry *slot = &p->cache[0];
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < CACHE_SLOTS; i++) {
            CacheEntry *e = &p->cache[i];
            if (!strcmp(e->ticket.key, t->key)) {
                slot = e;
                break;
            }
            bool expired = e->ticket.generation != p->generation || e->ticket.expires_ms <= now;
            uint64_t priority = expired ? 0 : e->last_used;
            if (priority < oldest) {
                oldest = priority;
                slot = e;
            }
        }
        slot->ticket = *t;
        slot->last_used = ++p->accesses;
        slot->header_len = header->length;
        slot->body_len = length;
        memcpy(slot->header, header->data, header->length);
        memcpy(slot->body, body, length);
    }
    pthread_mutex_unlock(&p->lock);
}

void proxy_cache_invalidate(ProxyPolicy *p)
{
    pthread_mutex_lock(&p->lock);
    p->generation++;
    pthread_mutex_unlock(&p->lock);
}
