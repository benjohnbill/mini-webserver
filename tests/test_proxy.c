#include "proxy.h"
#include "test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void test_absolute_uri(void)
{
    static const struct {
        const char *uri;
        const char *host;
        const char *port;
        const char *path;
    } valid[] = {
        {"http://example.com/path", "example.com", "80", "/path"},
        {"HTTP://example.com:8001/path?q=1", "example.com", "8001", "/path?q=1"},
        {"http://example.com", "example.com", "80", "/"},
        {"http://example.com?q=a@b", "example.com", "80", "/?q=a@b"},
        {"http://[::1]:8001/path", "::1", "8001", "/path"},
        {"http://[::1]", "::1", "80", "/"}
    };
    char host[256], port[8], path[HTTP_MAX_URI];
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
        int result = parse_absolute_uri(valid[i].uri, host, sizeof host, port, sizeof port,
                                        path, sizeof path);
        CHECK(result == 0);
        if (result == 0) {
            CHECK_STR(host, valid[i].host);
            CHECK_STR(port, valid[i].port);
            CHECK_STR(path, valid[i].path);
        }
    }

    const char *invalid[] = {
        "https://example.com/path", "http://user:pass@example.com/", "http:///path",
        "http://example.com:", "http://example.com:0/", "http://example.com:70000/",
        "http://example.com:+80/", "http://[::1/path", "http://::1/path",
        "http://[example.com]/", "http://[::1]extra/"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        CHECK(parse_absolute_uri(invalid[i], host, sizeof host, port, sizeof port,
                                 path, sizeof path) == -1);

    const char *uri = "http://example.com:8001/path";
    CHECK(parse_absolute_uri(uri, host, 11, port, sizeof port, path, sizeof path) == -1);
    CHECK(parse_absolute_uri(uri, host, sizeof host, port, 4, path, sizeof path) == -1);
    CHECK(parse_absolute_uri(uri, host, sizeof host, port, sizeof port, path, 5) == -1);
}

static void test_build_request(void)
{
    const http_header incoming[] = {
        {"Host", "old.example"}, {"Connection", "Keep-Alive, \t x-Drop \t, Upgrade"},
        {"X-Drop", "remove"}, {"connection", "X-Other"}, {"X-Other", "remove"},
        {"Keep-Alive", "timeout=5"}, {"Proxy-Connection", "keep-alive"},
        {"Proxy-Authorization", "secret"}, {"TE", "trailers"}, {"Trailer", "X-Trailer"},
        {"Transfer-Encoding", "chunked"}, {"Upgrade", "websocket"},
        {"Content-Length", "8"}, {"x-forwarded-for", "192.0.2.1"},
        {"X-Forwarded-For", "192.0.2.2"}, {"Content-Type", "text/plain"}, {"X-Dropper", "keep"}
    };
    http_request req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "POST");
    memcpy(req.headers, incoming, sizeof incoming);
    req.header_count = sizeof incoming / sizeof incoming[0];
    char body[] = {'a', '\0', 'b'};
    req.body = body;
    req.body_len = sizeof body;

    const char *host = "new.example:8001";
    const char *ip = "198.51.100.1";
    const char expected[] =
        "POST /post?q=1 HTTP/1.1\r\nContent-Type: text/plain\r\nX-Dropper: keep\r\n"
        "Host: new.example:8001\r\nConnection: close\r\n"
        "X-Forwarded-For: 192.0.2.1, 192.0.2.2, 198.51.100.1\r\n"
        "Content-Length: 3\r\n\r\na\0b";
    char out[sizeof expected - 1];
    CHECK(proxy_build_request(&req, "/post?q=1", host, ip, out, sizeof out) == (long)sizeof out);
    CHECK(memcmp(out, expected, sizeof out) == 0);
    CHECK(proxy_build_request(&req, "/post?q=1", host, ip, out, sizeof out - 1) == -1);
    CHECK(proxy_build_request(&req, "/post?q=1", host, ip, out, 5) == -1);

    req.header_count = 0;
    req.body = NULL;
    req.body_len = 0;
    CHECK(proxy_build_request(&req, "/", host, NULL, out, sizeof out) > 0);
    CHECK_STR(out, "POST / HTTP/1.1\r\nHost: new.example:8001\r\nConnection: close\r\n"
                   "Content-Length: 0\r\n\r\n");
}

static const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi";

typedef struct {
    int listener;
    int silent;
    char request[512];
} backend_thread;

static ssize_t receive(int fd, char *buf, size_t size)
{
    ssize_t n;
    do {
        n = recv(fd, buf, size, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

static void *serve_backend(void *arg)
{
    backend_thread *server = arg;
    int fd;
    do {
        fd = accept(server->listener, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return NULL;

    /* Bound the test even if the proxy fails to finish the request. */
    struct timeval timeout = {.tv_sec = 3};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0) {
        size_t used = 0;
        while (used < sizeof server->request - 1) {
            ssize_t n = receive(fd, server->request + used, sizeof server->request - 1 - used);
            if (n <= 0)
                break;
            used += (size_t)n;
            server->request[used] = '\0';
            if (strstr(server->request, "\r\n\r\n")) {
                if (server->silent) {
                    char extra;
                    (void)receive(fd, &extra, sizeof extra);
                } else {
                    for (size_t sent = 0; sent < sizeof response - 1;) {
                        ssize_t bytes = send(fd, response + sent, sizeof response - 1 - sent,
                                             MSG_NOSIGNAL);
                        if (bytes < 0 && errno == EINTR)
                            continue;
                        if (bytes <= 0)
                            break;
                        sent += (size_t)bytes;
                    }
                }
                break;
            }
        }
    }
    close(fd);
    return NULL;
}

static int bind_loopback(backend *b)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof addr;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    strcpy(b->host, "127.0.0.1");
    snprintf(b->port, sizeof b->port, "%u", (unsigned int)ntohs(addr.sin_port));
    return fd;
}

static void test_loopback(int silent)
{
    backend b;
    int listener = bind_loopback(&b);
    CHECK(listener >= 0);
    if (listener < 0)
        return;

    struct timeval accept_timeout = {.tv_sec = 3};
    int listening = setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout,
                              sizeof accept_timeout) == 0 && listen(listener, 1) == 0;
    CHECK(listening);
    int client[2];
    int paired = listening && socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0;
    CHECK(paired);
    if (!paired) {
        close(listener);
        return;
    }

    backend_thread server = {.listener = listener, .silent = silent};
    pthread_t thread;
    int started = pthread_create(&thread, NULL, serve_backend, &server) == 0;
    CHECK(started);
    if (started) {
        http_request req;
        memset(&req, 0, sizeof req);
        strcpy(req.method, "GET");
        strcpy(req.uri, "/health?q=1");
        CHECK(proxy_reverse(client[0], &req, &b, silent ? 300 : 2000, "198.51.100.2") ==
              (silent ? 504 : 0));
        CHECK(pthread_join(thread, NULL) == 0);
        char expected[512];
        snprintf(expected, sizeof expected,
                 "GET /health?q=1 HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nConnection: close\r\n"
                 "X-Forwarded-For: 198.51.100.2\r\n\r\n", b.port);
        CHECK_STR(server.request, expected);
    }
    close(client[0]);

    if (started) {
        char received[sizeof response + 1] = {0};
        size_t used = 0;
        ssize_t n;
        while (used < sizeof received - 1 &&
               (n = receive(client[1], received + used, sizeof received - 1 - used)) > 0)
            used += (size_t)n;
        CHECK(used == (silent ? 0 : sizeof response - 1));
        CHECK_STR(received, silent ? "" : response);
    }
    close(client[1]);
    close(listener);
}

static void test_connection_refused(void)
{
    backend b;
    int fd = bind_loopback(&b);
    CHECK(fd >= 0);
    if (fd < 0)
        return;

    http_request req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "GET");
    strcpy(req.uri, "/");
    /* The bound port stays reserved without accepting connections. */
    CHECK(proxy_reverse(-1, &req, &b, 300, NULL) == 502);
    close(fd);
}

int main(void)
{
    test_absolute_uri();
    test_build_request();
    test_loopback(0);
    test_connection_refused();
    test_loopback(1);
    TEST_MAIN_END();
}
