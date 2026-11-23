#include <stdio.h>
#include <string.h>

#include "check.h"
#include "forward.h"
#include "http_parse.h"

#define JA4_SAMPLE "t13d1516h2_8daaf6152771_e5627efa2ab1"
#define BUFFER_SIZE 2048

static void request_parse(char *text, struct request *request)
{
    CHECK_EQ_INT(http_parse_request(text, strlen(text), request), 0);
}

static void a_get_keeps_host_and_user_agent_and_adds_the_proxy_headers(void)
{
    char text[] = "GET /a?b=1 HTTP/1.1\r\nHost: example.com\r\n"
                  "User-Agent: curl/8.18.0\r\nAccept: */*\r\n\r\n";
    struct request request;
    request_parse(text, &request);
    char buffer[BUFFER_SIZE];
    size_t len = forward_format_request(&request, "203.0.113.7", JA4_SAMPLE,
                                        buffer, sizeof(buffer));
    const char *expected = "GET /a?b=1 HTTP/1.1\r\n"
                           "Host: example.com\r\n"
                           "User-Agent: curl/8.18.0\r\n"
                           "X-Forwarded-For: 203.0.113.7\r\n"
                           "X-JA4: " JA4_SAMPLE "\r\n"
                           "Connection: close\r\n"
                           "\r\n";
    CHECK_EQ_INT(len, strlen(expected));
    CHECK(memcmp(buffer, expected, len) == 0);
}

static void hop_by_hop_headers_of_the_client_do_not_reach_the_origin(void)
{
    char text[] = "GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n"
                  "Keep-Alive: timeout=5\r\nUpgrade: h2c\r\n\r\n";
    struct request request;
    request_parse(text, &request);
    char buffer[BUFFER_SIZE];
    size_t len =
        forward_format_request(&request, "::1", "", buffer, sizeof(buffer));
    const char *expected = "GET / HTTP/1.1\r\n"
                           "Host: a\r\n"
                           "X-Forwarded-For: ::1\r\n"
                           "X-JA4: \r\n"
                           "Connection: close\r\n"
                           "\r\n";
    CHECK_EQ_INT(len, strlen(expected));
    CHECK(memcmp(buffer, expected, len) == 0);
}

static void a_post_keeps_its_type_length_and_body(void)
{
    char text[] = "POST /echo HTTP/1.1\r\nHost: a\r\n"
                  "Content-Type: text/plain\r\nContent-Length: 3\r\n\r\nabc";
    struct request request;
    request_parse(text, &request);
    char buffer[BUFFER_SIZE];
    size_t len = forward_format_request(&request, "127.0.0.1", JA4_SAMPLE,
                                        buffer, sizeof(buffer));
    const char *expected = "POST /echo HTTP/1.1\r\n"
                           "Host: a\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 3\r\n"
                           "X-Forwarded-For: 127.0.0.1\r\n"
                           "X-JA4: " JA4_SAMPLE "\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "abc";
    CHECK_EQ_INT(len, strlen(expected));
    CHECK(memcmp(buffer, expected, len) == 0);
}

static void an_http_1_0_request_without_host_gets_no_host_line(void)
{
    char text[] = "GET / HTTP/1.0\r\n\r\n";
    struct request request;
    request_parse(text, &request);
    char buffer[BUFFER_SIZE];
    size_t len = forward_format_request(&request, "127.0.0.1", "", buffer,
                                        sizeof(buffer));
    const char *expected = "GET / HTTP/1.0\r\n"
                           "X-Forwarded-For: 127.0.0.1\r\n"
                           "X-JA4: \r\n"
                           "Connection: close\r\n"
                           "\r\n";
    CHECK_EQ_INT(len, strlen(expected));
    CHECK(memcmp(buffer, expected, len) == 0);
}

static void a_request_that_does_not_fit_gives_0(void)
{
    char text[] = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    struct request request;
    request_parse(text, &request);
    char buffer[40];
    CHECK_EQ_INT(forward_format_request(&request, "127.0.0.1", JA4_SAMPLE,
                                        buffer, sizeof(buffer)),
                 0);
}

static void origins_get_the_requests_in_turn(void)
{
    struct origin_list list = {.count = 3};
    atomic_init(&list.next, 0);
    CHECK_EQ_INT(origin_list_pick(&list), 0);
    CHECK_EQ_INT(origin_list_pick(&list), 1);
    CHECK_EQ_INT(origin_list_pick(&list), 2);
    CHECK_EQ_INT(origin_list_pick(&list), 0);
}

static void one_origin_gets_every_request(void)
{
    struct origin_list list = {.count = 1};
    atomic_init(&list.next, 0);
    CHECK_EQ_INT(origin_list_pick(&list), 0);
    CHECK_EQ_INT(origin_list_pick(&list), 0);
}

static void an_origin_parses_into_host_and_port(void)
{
    struct origin origin;
    CHECK_EQ_INT(origin_parse("127.0.0.1:18080", &origin), 0);
    CHECK(strcmp(origin.host, "127.0.0.1") == 0);
    CHECK(strcmp(origin.port, "18080") == 0);
}

static void a_bad_origin_is_rejected(void)
{
    static const char *bad[] = {
        "",       "host",       "host:",   ":80",
        "host:0", "host:65536", "host:8x", "host:80:81"};
    struct origin origin;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (origin_parse(bad[i], &origin) != -1) {
            fprintf(stderr, "origin_parse(\"%s\") was accepted\n", bad[i]);
            CHECK(0);
        }
    }
}

int main(void)
{
    a_get_keeps_host_and_user_agent_and_adds_the_proxy_headers();
    hop_by_hop_headers_of_the_client_do_not_reach_the_origin();
    a_post_keeps_its_type_length_and_body();
    an_http_1_0_request_without_host_gets_no_host_line();
    a_request_that_does_not_fit_gives_0();
    origins_get_the_requests_in_turn();
    one_origin_gets_every_request();
    an_origin_parses_into_host_and_port();
    a_bad_origin_is_rejected();
    printf("test_forward: ok\n");

    return 0;
}
