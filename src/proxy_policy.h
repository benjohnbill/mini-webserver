#ifndef PROXY_POLICY_H
#define PROXY_POLICY_H

#include "handlers.h"

enum { RELAY_CHUNK = 8192, CACHE_KEY_SIZE = 512, CACHE_HEADER_SIZE = 2048 };

typedef struct {
    char key[CACHE_KEY_SIZE];
    uint64_t generation;
    int64_t stored_ms, expires_ms;
    unsigned initial_age;
} CacheTicket;

ProxyPolicy *proxy_policy_new(void);
void proxy_policy_free(ProxyPolicy *policy);
bool proxy_rate_allow(ProxyPolicy *policy, struct in_addr ip, uint16_t rate, uint16_t burst);
bool proxy_cache_key(CacheTicket *ticket, const Request *req, const Backend *backend);
bool proxy_cache_get(ProxyPolicy *policy, CacheTicket *ticket, HttpOutput *header,
                     unsigned char body[RELAY_CHUNK], size_t *length);
bool proxy_cache_admit(CacheTicket *ticket, const UpstreamResponse *upstream, int64_t started_ms);
void proxy_cache_put(ProxyPolicy *policy, const CacheTicket *ticket, const HttpOutput *header,
                     const unsigned char *body, size_t length);
void proxy_cache_invalidate(ProxyPolicy *policy);

#endif
