#include <stdio.h>
#include <string.h>

#include "check.h"
#include "http_parse.h"

#define LONG_BUFFER_SIZE 9000

static size_t request_with_target(char *buffer, size_t size, size_t target_len);
static size_t request_with_header_len(char *buffer, size_t size,
                                      size_t header_len);

static void a_full_get_is_complete(void)
{
    char buffer[] = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(strcmp(request.method, "GET") == 0);
    CHECK(strcmp(request.target, "/index.html") == 0);
    CHECK(strcmp(request.version, "HTTP/1.1") == 0);
    CHECK(strcmp(request.host, "localhost") == 0);
    CHECK_EQ_INT(request.content_length, -1);
    CHECK_EQ_INT(request.body_len, 0);
    CHECK(!request.wants_close);
}

static void a_request_with_no_empty_line_is_incomplete(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request),
                 HTTP_INCOMPLETE);
    CHECK_EQ_INT(http_frame_measure(buffer, strlen(buffer)), HTTP_INCOMPLETE);
}

static void a_request_with_bare_newline_line_ends_is_complete(void)
{
    char buffer[] = "GET / HTTP/1.1\nHost: a\n\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(strcmp(request.host, "a") == 0);
}

static void an_unknown_version_gets_505(void)
{
    char buffer[] = "GET / HTTP/2.0\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 505);
}

static void an_unknown_method_gets_405(void)
{
    char buffer[] = "DELETE / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 405);
}

static void a_request_line_with_no_target_gets_400(void)
{
    char buffer[] = "GET HTTP/1.1\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void a_target_that_does_not_start_with_slash_gets_400(void)
{
    char buffer[] = "GET index.html HTTP/1.1\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void a_target_of_2048_bytes_passes(void)
{
    char buffer[LONG_BUFFER_SIZE];
    struct request request;
    size_t len = request_with_target(buffer, sizeof(buffer), 2048);
    CHECK_EQ_INT(http_parse_request(buffer, len, &request), 0);
    CHECK_EQ_INT(strlen(request.target), 2048);
}

static void a_target_over_2048_bytes_gets_414(void)
{
    char buffer[LONG_BUFFER_SIZE];
    struct request request;
    size_t len = request_with_target(buffer, sizeof(buffer), 2049);
    CHECK_EQ_INT(http_parse_request(buffer, len, &request), 414);
}

static void a_long_target_gets_414_when_the_request_line_ends(void)
{
    char buffer[LONG_BUFFER_SIZE];
    size_t len = request_with_target(buffer, sizeof(buffer), 2049);
    char *line_end = strchr(buffer, '\n');
    len = (size_t)(line_end - buffer) + 1;
    CHECK_EQ_INT(http_frame_measure(buffer, len), -414);
}

static void a_header_of_8192_bytes_is_complete(void)
{
    char buffer[LONG_BUFFER_SIZE];
    struct request request;
    size_t len = request_with_header_len(buffer, sizeof(buffer), 8192);
    CHECK_EQ_INT(http_frame_measure(buffer, len), 8192);
    CHECK_EQ_INT(http_parse_request(buffer, len, &request), 0);
}

static void a_header_over_8192_bytes_gets_431(void)
{
    char buffer[LONG_BUFFER_SIZE];
    struct request request;
    size_t len = request_with_header_len(buffer, sizeof(buffer), 8193);
    CHECK_EQ_INT(http_frame_measure(buffer, len), -431);
    CHECK_EQ_INT(http_parse_request(buffer, len, &request), 431);
}

static void a_request_line_with_no_end_in_8192_bytes_gets_431(void)
{
    char buffer[LONG_BUFFER_SIZE];
    memset(buffer, 'a', 8192);
    memcpy(buffer, "GET /", 5);
    CHECK_EQ_INT(http_frame_measure(buffer, 8192), -431);
    CHECK_EQ_INT(http_frame_measure(buffer, 8191), HTTP_INCOMPLETE);
}

static void a_post_with_no_content_length_gets_411(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 411);
}

static void two_content_length_headers_get_400(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n"
                    "Content-Length: 1\r\n\r\nx";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void a_content_length_that_is_not_decimal_gets_400(void)
{
    char buffer[] =
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void transfer_encoding_gets_400(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void a_body_of_1048576_bytes_passes_the_frame_check(void)
{
    char buffer[] =
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1048576\r\n\r\n";
    CHECK_EQ_INT(http_frame_measure(buffer, strlen(buffer)), 1048633);
}

static void a_body_over_1048576_bytes_gets_413_before_the_body(void)
{
    char buffer[] =
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1048577\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_frame_measure(buffer, strlen(buffer)), -413);
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 413);
}

static void the_frame_check_gives_the_total_length_before_the_body(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                    "\r\nhel";
    struct request request;
    CHECK_EQ_INT(http_frame_measure(buffer, strlen(buffer)), 56);
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request),
                 HTTP_INCOMPLETE);
}

static void a_post_with_its_full_body_is_complete(void)
{
    char buffer[] = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                    "Content-Type: text/plain\r\n\r\nhelloEXTRA";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK_EQ_INT(request.content_length, 5);
    CHECK_EQ_INT(request.body_len, 5);
    CHECK(memcmp(request.body, "hello", 5) == 0);
    CHECK(strcmp(request.content_type, "text/plain") == 0);
}

static void the_frame_check_does_not_change_the_buffer(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n";
    char before[sizeof(buffer)];
    memcpy(before, buffer, sizeof(buffer));
    CHECK_EQ_INT(http_frame_measure(buffer, strlen(buffer)), 46);
    CHECK(memcmp(before, buffer, sizeof(buffer)) == 0);
}

static void connection_close_sets_wants_close(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\nconnection: Close\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 0);
    CHECK(request.wants_close);
}

static void a_header_line_with_no_colon_gets_400(void)
{
    char buffer[] = "GET / HTTP/1.1\r\nHost: a\r\nbroken\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void a_bad_header_line_gets_400_before_an_unknown_version(void)
{
    char buffer[] = "GET / HTTP/2.0\r\nHost: a\r\nbroken\r\n\r\n";
    struct request request;
    CHECK_EQ_INT(http_parse_request(buffer, strlen(buffer), &request), 400);
}

static void an_unknown_method_waits_for_the_header_end(void)
{
    char buffer[] = "DELETE / HTTP/1.1\r\nHost: a\r\n";
    CHECK_EQ_INT(http_frame_measure(buffer, strlen(buffer)), HTTP_INCOMPLETE);
}

int main(void)
{
    a_full_get_is_complete();
    a_request_with_no_empty_line_is_incomplete();
    a_request_with_bare_newline_line_ends_is_complete();
    an_unknown_version_gets_505();
    an_unknown_method_gets_405();
    a_request_line_with_no_target_gets_400();
    a_target_that_does_not_start_with_slash_gets_400();
    a_target_of_2048_bytes_passes();
    a_target_over_2048_bytes_gets_414();
    a_long_target_gets_414_when_the_request_line_ends();
    a_header_of_8192_bytes_is_complete();
    a_header_over_8192_bytes_gets_431();
    a_request_line_with_no_end_in_8192_bytes_gets_431();
    a_post_with_no_content_length_gets_411();
    two_content_length_headers_get_400();
    a_content_length_that_is_not_decimal_gets_400();
    transfer_encoding_gets_400();
    a_body_of_1048576_bytes_passes_the_frame_check();
    a_body_over_1048576_bytes_gets_413_before_the_body();
    the_frame_check_gives_the_total_length_before_the_body();
    a_post_with_its_full_body_is_complete();
    the_frame_check_does_not_change_the_buffer();
    connection_close_sets_wants_close();
    a_header_line_with_no_colon_gets_400();
    a_bad_header_line_gets_400_before_an_unknown_version();
    an_unknown_method_waits_for_the_header_end();
    printf("test_http_parse: ok\n");
    return 0;
}

/* The target is "/" and then (target_len - 1) letters. */
static size_t request_with_target(char *buffer, size_t size, size_t target_len)
{
    char target[LONG_BUFFER_SIZE];
    memset(target, 'a', target_len);
    target[0] = '/';
    target[target_len] = '\0';
    int len =
        snprintf(buffer, size, "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", target);
    return (size_t)len;
}

/* Pads an X-Pad header so that the header section is header_len bytes. */
static size_t request_with_header_len(char *buffer, size_t size,
                                      size_t header_len)
{
    const char *head = "GET / HTTP/1.1\r\nHost: a\r\nX-Pad: ";
    const char *tail = "\r\n\r\n";
    size_t pad_len = header_len - strlen(head) - strlen(tail);
    char pad[LONG_BUFFER_SIZE];
    memset(pad, 'p', pad_len);
    pad[pad_len] = '\0';
    int len = snprintf(buffer, size, "%s%s%s", head, pad, tail);
    return (size_t)len;
}
