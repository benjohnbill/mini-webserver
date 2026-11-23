#include "router.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "config.h"
#include "handlers.h"

struct handler_route {
    const char *target;
    const char *method;
    int (*handler)(const struct request *request, struct response_plan *plan);
};

static const struct handler_route handler_routes[] = {
    {"/echo", "POST", handlers_echo},
    {"/api/time", "GET", handlers_tell_time},
};

static bool target_has_dot_dot_segment(const char *target);
static const struct handler_route *handler_route_find(const char *target);

int router_resolve(const struct request *request, struct route *route)
{
    if (target_has_dot_dot_segment(request->target)) {
        return 403;
    }

    const struct handler_route *handler_route =
        handler_route_find(request->target);
    if (handler_route != NULL) {
        route->kind = ROUTE_HANDLER;
        route->allowed_method = handler_route->method;
        route->handler = handler_route->handler;
        return strcmp(request->method, route->allowed_method) == 0 ? 0 : 405;
    }

    route->kind = ROUTE_STATIC;
    route->allowed_method = "GET";
    route->handler = NULL;

    return strcmp(request->method, "GET") == 0 ? 0 : 405;
}

static bool target_has_dot_dot_segment(const char *target)
{
    for (const char *slash = strchr(target, '/'); slash != NULL;
         slash = strchr(slash + 1, '/')) {
        if (strncmp(slash, "/..", 3) == 0 &&
            (slash[3] == '/' || slash[3] == '\0')) {
            return true;
        }
    }

    return false;
}

static const struct handler_route *handler_route_find(const char *target)
{
    for (size_t i = 0; i < sizeof(handler_routes) / sizeof(handler_routes[0]);
         i++) {
        if (strcmp(target, handler_routes[i].target) == 0) {
            return &handler_routes[i];
        }
    }

    return NULL;
}
