#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* A folder with a.txt: 5 bytes, mtime 1700000000 (0x6553f100). */
static void etag_root_create(char *root)
{
    CHECK(mkdtemp(root) != NULL);
    char path[256];
    snprintf(path, sizeof(path), "%s/a.txt", root);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    fputs("hello", file);
    fclose(file);
    struct timespec times[2] = {{.tv_sec = 1700000000}, {.tv_sec = 1700000000}};
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
}

static void etag_root_destroy(const char *root)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/a.txt", root);
    unlink(path);
    rmdir(root);
}

static void a_static_file_has_an_etag_of_its_mtime_and_size(void)
{
    char root[] = "/tmp/test_serve.XXXXXX";
    etag_root_create(root);
    char buffer[] = "GET /a.txt HTTP/1.1\r\nHost: a\r\n\r\n";
    struct config config = {.root = root};
    struct request request;
    struct response_plan plan;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    serve_request(&request, &config, &plan);
    CHECK(strstr(plan.header, "HTTP/1.1 200 OK\r\n") == plan.header);
    CHECK(strstr(plan.header, "\r\nETag: \"6553f100-5\"\r\n") != NULL);
    response_plan_close(&plan);
    etag_root_destroy(root);
}

static void a_handler_reply_has_no_etag(void)
{
    char buffer[] = "GET /api/time HTTP/1.1\r\nHost: a\r\n\r\n";
    struct response_plan plan;
    plan_for(buffer, &plan);
    CHECK(strstr(plan.header, "ETag") == NULL);
    response_plan_close(&plan);
}

static void an_error_reply_has_no_etag(void)
{
    char buffer[] = "GET /missing.html HTTP/1.1\r\nHost: a\r\n\r\n";
    struct response_plan plan;
    plan_for(buffer, &plan);
    CHECK(strstr(plan.header, "HTTP/1.1 404 ") == plan.header);
    CHECK(strstr(plan.header, "ETag") == NULL);
    response_plan_close(&plan);
}

int main(void)
{
    a_static_file_has_an_etag_of_its_mtime_and_size();
    a_handler_reply_has_no_etag();
    an_error_reply_has_no_etag();
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
