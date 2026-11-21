#include <stdio.h>
#include <string.h>

#include "check.h"
#include "router.h"

static void the_root_is_static(void)
{
    struct request request = {.method = "GET", .target = "/"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 0);
    CHECK_EQ_INT(route.kind, ROUTE_STATIC);
}

static void echo_with_post_is_a_handler(void)
{
    struct request request = {.method = "POST", .target = "/echo"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 0);
    CHECK_EQ_INT(route.kind, ROUTE_HANDLER);
    CHECK(route.handler != NULL);
}

static void api_time_with_get_is_a_handler(void)
{
    struct request request = {.method = "GET", .target = "/api/time"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 0);
    CHECK_EQ_INT(route.kind, ROUTE_HANDLER);
}

static void echo_with_get_gets_405_and_allows_post(void)
{
    struct request request = {.method = "GET", .target = "/echo"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 405);
    CHECK(strcmp(route.allowed_method, "POST") == 0);
}

static void a_static_file_with_post_gets_405_and_allows_get(void)
{
    struct request request = {.method = "POST", .target = "/index.html"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 405);
    CHECK(strcmp(route.allowed_method, "GET") == 0);
}

static void a_dot_dot_segment_gets_403(void)
{
    struct request request = {.method = "GET", .target = "/../etc/passwd"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 403);
}

static void a_dot_dot_at_the_end_gets_403(void)
{
    struct request request = {.method = "GET", .target = "/css/.."};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 403);
}

static void dots_inside_a_name_are_not_a_dot_dot_segment(void)
{
    struct request request = {.method = "GET", .target = "/a..b/..c"};
    struct route route;
    CHECK_EQ_INT(router_resolve(&request, &route), 0);
}

int main(void)
{
    the_root_is_static();
    echo_with_post_is_a_handler();
    api_time_with_get_is_a_handler();
    echo_with_get_gets_405_and_allows_post();
    a_static_file_with_post_gets_405_and_allows_get();
    a_dot_dot_segment_gets_403();
    a_dot_dot_at_the_end_gets_403();
    dots_inside_a_name_are_not_a_dot_dot_segment();
    printf("test_router: ok\n");
    return 0;
}
