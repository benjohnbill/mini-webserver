#ifndef HANDLERS_H
#define HANDLERS_H

#include "http.h"

#include <arpa/inet.h>

#define ROUTE_LIMIT 32

typedef struct {
    char prefix[HTTP_TARGET_LIMIT + 1];
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
} Route;

typedef struct {
    int root_fd;
    Route routes[ROUTE_LIMIT];
    size_t route_count;
} ServerConfig;

bool parse_port(const char *value, uint16_t *port);
bool parse_route(const char *value, Route *route);
const Route *match_route(const ServerConfig *config, const char *path);
const char *mime_type_for(const char *path);
void handle_client(int fd, const ServerConfig *config);

#endif
