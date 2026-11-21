#include "handlers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void parse_request(const char *input, size_t length, int expected_status)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(write(sockets[0], input, length) == (ssize_t)length);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    assert(net_nonblocking(sockets[1]) == 0);

    Stream stream;
    stream_init(&stream, sockets[1], net_deadline());
    Request request = {0};
    int status = http_read_request(&stream, &request);
    assert(status == expected_status);
    close(sockets[0]);
    close(sockets[1]);

    if (status == 0) {
        assert(strcmp(request.method, "POST") == 0);
        assert(strcmp(request.target, "/echo?q=1") == 0);
        assert(strcmp(request.path, "/echo") == 0);
        assert(strcmp(request.version, "HTTP/1.1") == 0);
        assert(request.head.length == 3);
        assert(memcmp(request.body, "a\0b", 3) == 0);
    }
    free(request.body);
    free(request.path);
}

static void test_http(void)
{
    const char request[] = "POST /echo?q=1 HTTP/1.1\r\nhost: localhost\r\n"
                           "content-length: 3\r\n\r\na\0b";
    parse_request(request, sizeof request - 1, 0);

    const struct { const char *request; int status; } tests[] = {
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n", 400},
        {"GET / HTTP/9.9\r\nHost: localhost\r\n\r\n", 505},
        {"GET / HTTP/1.1\r\nHost: local", 400},
        {"GET / HTTP/1.1\r\nHost : local\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: local\r\nHost: local\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 1048577\r\n\r\n", 413},
        {"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\na", 400},
        {"POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        parse_request(tests[i].request, strlen(tests[i].request), tests[i].status);
}

static void test_routes(void)
{
    ServerConfig config = {.route_count = 3};
    assert(parse_route("/api/=127.0.0.1:8001", &config.routes[0]));
    assert(parse_route("/api/v2=127.0.0.1:8002", &config.routes[1]));
    assert(parse_route("/assets=127.0.0.1:8003", &config.routes[2]));
    assert(!parse_route("/api=localhost:8001", &(Route){0}));
    assert(!parse_route("/api=127.0.0.1:65536", &(Route){0}));
    assert(match_route(&config, "/api") == &config.routes[0]);
    assert(match_route(&config, "/api/v2/users") == &config.routes[1]);
    assert(match_route(&config, "/assets/logo.png") == &config.routes[2]);
    assert(match_route(&config, "/apiary") == NULL);

    HttpHead head = {.count = 1, .headers = {{"Connection", "Keep-Alive, X-Local"}}};
    assert(!http_forward_header(&head, "X-Local"));
    assert(!http_forward_header(&head, "content-length"));
    assert(http_forward_header(&head, "Content-Type"));
}

static void test_mime(void)
{
    assert(strcmp(mime_type_for("index.html"), "text/html; charset=utf-8") == 0);
    assert(strcmp(mime_type_for("style.css"), "text/css; charset=utf-8") == 0);
    assert(strcmp(mime_type_for("app.js"), "text/javascript; charset=utf-8") == 0);
    assert(strcmp(mime_type_for("logo.PNG"), "image/png") == 0);
    assert(strcmp(mime_type_for("photo.jpg"), "image/jpeg") == 0);
    assert(strcmp(mime_type_for("unknown"), "application/octet-stream") == 0);
}

int main(void)
{
    test_http();
    test_routes();
    test_mime();
    puts("Unit tests passed");
    return 0;
}
