/* Unit tests for the HTTP request parser (pure, no sockets). */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "server_run.h"
#include "test.h"

static http_request req;              /* large: keep it off the stack */

static parse_result parse(const char *s, size_t *head_len)
{
    size_t dummy;
    return http_parse_head(s, strlen(s), &req, head_len ? head_len : &dummy);
}

static void test_valid_get(void)
{
    const char *s = "GET /index.html?a=1&b=2 HTTP/1.1\r\n"
                    "Host: localhost:8080\r\n"
                    "User-Agent:   curl/8.0  \r\n"
                    "\r\n";
    size_t hl = 0;
    CHECK(parse(s, &hl) == PARSE_OK);
    CHECK(hl == strlen(s));
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.uri, "/index.html?a=1&b=2");
    CHECK_STR(req.path, "/index.html");
    CHECK_STR(req.query, "a=1&b=2");
    CHECK(req.version_minor == 1);
    CHECK(req.header_count == 2);
    CHECK_STR(http_get_header(&req, "host"), "localhost:8080");
    CHECK_STR(http_get_header(&req, "USER-AGENT"), "curl/8.0"); /* OWS trimmed */
    CHECK(http_get_header(&req, "Cookie") == NULL);
    CHECK(req.keep_alive == 1);
    CHECK(req.has_content_length == 0);
    CHECK(req.body == NULL);
}

static void test_post_and_pipelining(void)
{
    const char *s = "POST /form HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\n"
                    "hello=worldGET / HTTP/1.1\r\n";
    size_t hl = 0;
    CHECK(http_parse_head(s, strlen(s), &req, &hl) == PARSE_OK);
    CHECK_STR(req.method, "POST");
    CHECK(req.has_content_length == 1);
    CHECK(req.content_length == 11);
    CHECK(strncmp(s + hl, "hello=world", 11) == 0); /* body starts at head_len */

    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"
                "Content-Length: 3\r\n\r\n", NULL) == PARSE_OK);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"
                "Content-Length: 4\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n",
                NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n",
                NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n",
                NULL) == PARSE_BODY_TOO_LARGE);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n",
                NULL) == PARSE_BODY_TOO_LARGE);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                NULL) == PARSE_NOT_IMPLEMENTED);
}

static void test_host_and_version(void)
{
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", NULL) == PARSE_OK); /* 1.0: optional */
    CHECK(req.version_minor == 0);

    CHECK(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_VERSION_NOT_SUPPORTED);
    CHECK(parse("GET / HTTP/1.2\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_VERSION_NOT_SUPPORTED);
    CHECK(parse("GET / HTTP/1\r\nHost: x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / FOO/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("GET /\r\nHost: x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
}

static void test_methods(void)
{
    CHECK(parse("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_OK);
    CHECK(parse("DELETE / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_NOT_IMPLEMENTED);
    CHECK(parse("BREW / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_NOT_IMPLEMENTED);
    CHECK(http_parse_status(PARSE_NOT_IMPLEMENTED) == 501);
    /* syntactically broken method token -> 400, not 501 */
    CHECK(parse("G(T / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse(" GET / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("GET  / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
}

static void test_bad_headers(void)
{
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nNoColon\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost : x\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n: empty\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX: a\rb\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX-Empty:\r\n\r\n", NULL) == PARSE_OK);
    CHECK_STR(http_get_header(&req, "x-empty"), "");

    /* too many header fields -> 431 */
    char big[HTTP_MAX_HEAD_BYTES];
    size_t o = (size_t)snprintf(big, sizeof big, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "X-%d: v\r\n", i);
    o += (size_t)snprintf(big + o, sizeof big - o, "\r\n");
    CHECK(o < sizeof big);
    CHECK(parse(big, NULL) == PARSE_HEADERS_TOO_LARGE);
}

static void test_long_uri(void)
{
    static char buf[HTTP_MAX_HEAD_BYTES + 16];
    char *p = buf;
    /* complete request whose target is longer than HTTP_MAX_URI -> 414 */
    p += sprintf(p, "GET /");
    memset(p, 'a', HTTP_MAX_URI + 10);
    p += HTTP_MAX_URI + 10;
    sprintf(p, " HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(parse(buf, NULL) == PARSE_URI_TOO_LONG);
    CHECK(http_parse_status(PARSE_URI_TOO_LONG) == 414);

    /* full buffer without CRLFCRLF: 414 if the request line itself is the
     * problem, 431 if the headers are */
    memset(buf, 'a', sizeof buf);
    memcpy(buf, "GET /", 5);
    size_t hl;
    CHECK(http_parse_head(buf, HTTP_MAX_HEAD_BYTES, &req, &hl) ==
          PARSE_URI_TOO_LONG);
    int n = sprintf(buf, "GET / HTTP/1.1\r\nX: ");
    memset(buf + n, 'b', sizeof buf - (size_t)n);
    CHECK(http_parse_head(buf, HTTP_MAX_HEAD_BYTES, &req, &hl) ==
          PARSE_HEADERS_TOO_LARGE);
}

static void test_percent_decoding(void)
{
    CHECK(parse("GET /a%20b/%E2%9C%93.html?q=%20 HTTP/1.1\r\nHost: x\r\n\r\n",
                NULL) == PARSE_OK);
    CHECK_STR(req.path, "/a b/\xE2\x9C\x93.html");
    CHECK_STR(req.query, "q=%20");           /* query stays raw */
    CHECK(parse("GET /a%00b HTTP/1.1\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET /a%zz HTTP/1.1\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET /a%4 HTTP/1.1\r\nHost: x\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);

    char out[8];
    CHECK(http_url_decode("%41%62c", out, sizeof out) == 0);
    CHECK_STR(out, "Abc");
    CHECK(http_url_decode("abcdefgh", out, sizeof out) != 0); /* no room */
    CHECK(http_url_decode("%", out, sizeof out) != 0);
}

static void test_targets(void)
{
    CHECK(parse("GET http://example.com:8080/x/y?z=1 HTTP/1.1\r\n"
                "Host: example.com\r\n\r\n", NULL) == PARSE_OK);
    CHECK_STR(req.uri, "http://example.com:8080/x/y?z=1");
    CHECK_STR(req.path, "/x/y");
    CHECK_STR(req.query, "z=1");
    CHECK(parse("GET http://example.com HTTP/1.1\r\nHost: e\r\n\r\n", NULL) ==
          PARSE_OK);
    CHECK_STR(req.path, "/");
    CHECK_STR(req.query, "");
    CHECK(parse("GET http:/// HTTP/1.1\r\nHost: e\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
    CHECK(parse("GET * HTTP/1.1\r\nHost: e\r\n\r\n", NULL) == PARSE_BAD_REQUEST);
    CHECK(parse("GET index.html HTTP/1.1\r\nHost: e\r\n\r\n", NULL) ==
          PARSE_BAD_REQUEST);
}

static void test_keep_alive(void)
{
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_OK);
    CHECK(req.keep_alive == 1);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", NULL) ==
          PARSE_OK);
    CHECK(req.keep_alive == 0);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, CLOSE\r\n\r\n",
                NULL) == PARSE_OK);
    CHECK(req.keep_alive == 0);
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", NULL) == PARSE_OK);
    CHECK(req.keep_alive == 0);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", NULL) ==
          PARSE_OK);
    CHECK(req.keep_alive == 1);
}

static void test_incomplete(void)
{
    CHECK(parse("", NULL) == PARSE_INCOMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\n", NULL) == PARSE_INCOMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n", NULL) == PARSE_INCOMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r", NULL) == PARSE_INCOMPLETE);
    /* bare LF line endings are not accepted as a terminator */
    CHECK(parse("GET / HTTP/1.1\nHost: x\n\n", NULL) == PARSE_INCOMPLETE);
    /* leading empty lines are ignored (RFC 7230 3.5) */
    CHECK(parse("\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", NULL) == PARSE_OK);
}

static void test_status_text(void)
{
    CHECK_STR(http_status_text(200), "OK");
    CHECK_STR(http_status_text(404), "Not Found");
    CHECK_STR(http_status_text(431), "Request Header Fields Too Large");
    CHECK_STR(http_status_text(505), "HTTP Version Not Supported");
}

/* Drive http_read_request over a real socket: a POST with a body followed
 * by a pipelined GET, all arriving in one burst. */
static void test_read_pipelined(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *wire = "POST /a?x=1 HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n"
                       "helloGET /b HTTP/1.1\r\nHost: h\r\n\r\n";
    CHECK(write_all(sv[1], wire, strlen(wire)) == (ssize_t)strlen(wire));

    static http_conn c;
    c.fd = sv[0];
    c.len = 0;
    CHECK(http_read_request(&c, &req) == 0);
    CHECK_STR(req.method, "POST");
    CHECK_STR(req.path, "/a");
    CHECK(req.body != NULL && req.body_len == 5);
    CHECK(req.body && memcmp(req.body, "hello", 5) == 0 && req.body[5] == '\0');
    http_request_free(&req);

    CHECK(http_read_request(&c, &req) == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.path, "/b");
    CHECK(req.body == NULL);
    CHECK(c.len == 0);
    http_request_free(&req);

    /* unsupported expectation -> 417 */
    const char *exp = "POST / HTTP/1.1\r\nHost: h\r\nExpect: magic\r\n"
                      "Content-Length: 1\r\n\r\nx";
    CHECK(write_all(sv[1], exp, strlen(exp)) == (ssize_t)strlen(exp));
    CHECK(http_read_request(&c, &req) == 417);
    http_request_free(&req);

    close(sv[1]);                       /* peer gone -> -1 */
    c.len = 0;
    CHECK(http_read_request(&c, &req) == -1);
    close(sv[0]);
}

static void test_dot_segments(void)
{
    CHECK(path_has_dot_segment("/api/../admin") == 1);
    CHECK(path_has_dot_segment("/api/./x") == 1);
    CHECK(path_has_dot_segment("/a/..") == 1);
    CHECK(path_has_dot_segment("/.") == 1);
    CHECK(path_has_dot_segment("/") == 0);
    CHECK(path_has_dot_segment("/index.html") == 0);
    CHECK(path_has_dot_segment("/.well-known/x") == 0);
    CHECK(path_has_dot_segment("/a..b/...") == 0);
}

int main(void)
{
    test_valid_get();
    test_post_and_pipelining();
    test_host_and_version();
    test_methods();
    test_bad_headers();
    test_long_uri();
    test_percent_decoding();
    test_targets();
    test_keep_alive();
    test_incomplete();
    test_status_text();
    test_read_pipelined();
    test_dot_segments();
    TEST_MAIN_END();
}
