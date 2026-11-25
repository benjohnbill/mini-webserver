#ifndef HANDLERS_H
#define HANDLERS_H

#include "http.h"

#include <arpa/inet.h>

#define ROUTE_LIMIT 32
#define BACKEND_LIMIT 8
#define KETAMA_POINTS 160

typedef struct {
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
} Backend;

typedef struct {
    uint32_t hash;
    unsigned backend;
} RingPoint;

typedef struct ProxyPolicy ProxyPolicy;

typedef struct {
    char prefix[HTTP_TARGET_LIMIT + 1];
    Backend backends[BACKEND_LIMIT];
    size_t count;
    RingPoint *ring;
} Route;

typedef struct {
    int root_fd;
    ProxyPolicy *policy;
    bool cache_enabled;
    uint16_t rate, burst;
    Route routes[ROUTE_LIMIT];
    size_t route_count;
} ServerConfig;

bool ketama_build(Route *route);
const Backend *ketama_select(const Route *route, struct in_addr peer);

bool parse_port(const char *value, uint16_t *port);
bool parse_route(const char *value, Route *route);
const Route *match_route(const ServerConfig *config, const char *path);
const char *mime_type_for(const char *path);
int open_static(int root_fd, const char *request_path, int *fd, size_t *length, const char **type);
int build_proxy_request(HttpOutput *out, const Backend *backend, const Request *request);
int build_proxy_response(HttpOutput *out, const UpstreamResponse *response);

#endif
