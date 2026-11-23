#include <stdio.h>
#include <string.h>

#include "check.h"
#include "serve.h"

static void http_1_0_closes(void)
{
    char buffer[] = "GET / HTTP/1.0\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(!serve_keeps_open(&request, 200));
}

static void http_1_0_with_keep_alive_keeps_the_connection_open(void)
{
    char buffer[] = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(serve_keeps_open(&request, 200));
}

static void http_1_1_keeps_the_connection_open(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(serve_keeps_open(&request, 200));
}

static void connection_close_closes(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(!serve_keeps_open(&request, 200));
}

static void an_error_closes(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(!serve_keeps_open(&request, 404));
}

static void echo_sends_back_the_body_and_its_content_type(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                    "Content-Type: text/plain\r\n\r\nhello";
    struct config config = {.root = "www"};
    struct request request;
    struct response_plan plan;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    serve_request(&request, &config, &plan);
    CHECK(strstr(plan.header, "HTTP/1.1 200 OK\r\n") == plan.header);
    CHECK(strstr(plan.header, "\r\nContent-Type: text/plain\r\n") != NULL);
    CHECK_EQ_INT(plan.body_len, 5);
    CHECK(memcmp(plan.body, "hello", 5) == 0);
    CHECK(!plan.close_after);
    response_plan_close(&plan);
}

static void plan_for(char *buffer, struct response_plan *plan)
{
    struct config config = {.root = "www"};
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    serve_request(&request, &config, plan);
}

static void the_time_handler_says_no_store(void)
{
    char buffer[] = "GET /api/time HTTP/1.1\r\nHost: a\r\n\r\n";
    struct response_plan plan;
    plan_for(buffer, &plan);
    CHECK(strstr(plan.header, "\r\nCache-Control: no-store\r\n") != NULL);
    response_plan_close(&plan);
}

static void the_echo_handler_says_no_store(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n"
                    "\r\nhi";
    struct response_plan plan;
    plan_for(buffer, &plan);
    CHECK(strstr(plan.header, "\r\nCache-Control: no-store\r\n") != NULL);
    response_plan_close(&plan);
}

static void a_static_file_has_no_cache_control(void)
{
    char buffer[] = "GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n";
    struct response_plan plan;
    plan_for(buffer, &plan);
    CHECK(strstr(plan.header, "HTTP/1.1 200 OK\r\n") == plan.header);
    CHECK(strstr(plan.header, "Cache-Control") == NULL);
    response_plan_close(&plan);
}

static void a_dot_dot_target_gets_a_403_plan_that_closes(void)
{
    char buffer[] = "GET /../secret HTTP/1.1\r\nHost: a\r\n\r\n";
    struct config config = {.root = "www"};
    struct request request;
    struct response_plan plan;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    serve_request(&request, &config, &plan);
    CHECK(strstr(plan.header, "HTTP/1.1 403 Forbidden\r\n") == plan.header);
    CHECK(plan.close_after);
    response_plan_close(&plan);
}

int main(void)
{
    http_1_0_closes();
    http_1_0_with_keep_alive_keeps_the_connection_open();
    http_1_1_keeps_the_connection_open();
    connection_close_closes();
    an_error_closes();
    echo_sends_back_the_body_and_its_content_type();
    a_dot_dot_target_gets_a_403_plan_that_closes();
    the_time_handler_says_no_store();
    the_echo_handler_says_no_store();
    a_static_file_has_no_cache_control();
    printf("test_serve: ok\n");

    return 0;
}
