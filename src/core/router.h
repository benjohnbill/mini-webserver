#ifndef ROUTER_H
#define ROUTER_H

#include "http_parse.h"
#include "response.h"

/* Pure: no files. */
enum route_kind { ROUTE_STATIC, ROUTE_HANDLER };

struct route {
    enum route_kind kind;
    const char *allowed_method;
    int (*handler)(const struct request *request, struct response_plan *plan);
};

/* Returns 0, 403, or 405. On 405, route->allowed_method is set. */
int router_resolve(const struct request *request, struct route *route);

#endif
