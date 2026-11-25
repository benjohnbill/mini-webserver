#include "handlers.h"
#include "proxy_policy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_proxy_flow(void);

static void parse_request(const char *input, size_t length, int expected_status)
{
    Request request = {0};
    int status = HTTP_MORE;
    /* Exercise incremental parsing even when a header delimiter is split. */
    for (size_t i = 0; i < length && status == HTTP_MORE; i++)
        status = http_feed_request(&request, (const unsigned char *)input + i, 1);
    assert(status == expected_status);

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
        {"GET / HTTP/1.1\r\nHost: local", HTTP_MORE},
        {"GET / HTTP/1.1\r\nHost : local\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: local\r\nHost: local\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 1048577\r\n\r\n", 413},
        {"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\na", HTTP_MORE},
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

static void test_ketama(void)
{
    Route two, three, reordered;
    assert(parse_route("/api=127.0.0.1:8001,127.0.0.1:8002", &two));
    assert(parse_route("/api@ketama=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003", &three));
    assert(parse_route("/api=127.0.0.1:8003,127.0.0.1:8002,127.0.0.1:8001", &reordered));
    assert(!strcmp(three.prefix, "/api"));
    unsigned owners[3] = {0};
    for (unsigned i = 0; i < 1024; i++) {
        char ip[INET_ADDRSTRLEN];
        snprintf(ip, sizeof ip, "10.0.%u.%u", i / 256, i % 256);
        struct in_addr peer;
        assert(inet_pton(AF_INET, ip, &peer) == 1);
        unsigned before = ketama_select(&two, peer)->port;
        unsigned after = ketama_select(&three, peer)->port;
        assert(after == ketama_select(&reordered, peer)->port);
        assert(after == before || after == 8003);
        owners[after - 8001]++;
    }
    /* Reference allocation computed with Python hashlib.md5 for these endpoints and IPs. */
    assert(owners[0] == 327 && owners[1] == 346 && owners[2] == 351);
    free(two.ring);
    free(three.ring);
    free(reordered.ring);
    assert(!parse_route("/api=127.0.0.1:80,127.0.0.1:80", &three));
    assert(!parse_route("/api=127.0.0.1:80,", &three));
}

static void test_shared_policy(void)
{
    ProxyPolicy *policy = proxy_policy_new();
    assert(policy);
    struct in_addr first, second;
    assert(inet_pton(AF_INET, "10.0.0.1", &first) == 1);
    assert(inet_pton(AF_INET, "10.0.0.2", &second) == 1);
    assert(proxy_rate_allow(policy, first, 1, 1));
    assert(!proxy_rate_allow(policy, first, 1, 1));
    assert(proxy_rate_allow(policy, second, 1, 1));

    /* Fill all cache slots, touch the oldest and admit one more entry. */
    unsigned char bytes[RELAY_CHUNK] = {'A'};
    HttpOutput header = {0}, found = {0};
    assert(http_make_head(&header, 200, "text/plain", 1, NULL) == 0);
    size_t length = 0;
    CacheTicket t = {.stored_ms = net_now_ms(), .expires_ms = net_now_ms() + 60000};
    for (int i = 0; i < 64; i++) {
        snprintf(t.key, sizeof t.key, "/item/%d", i);
        proxy_cache_put(policy, &t, &header, bytes, 1);
    }
    strcpy(t.key, "/item/0");
    assert(proxy_cache_get(policy, &t, &found, bytes, &length) && length == 1);
    strcpy(t.key, "/item/64");
    proxy_cache_put(policy, &t, &header, bytes, 1);
    strcpy(t.key, "/item/1");
    assert(!proxy_cache_get(policy, &t, &found, bytes, &length));
    strcpy(t.key, "/item/0");
    assert(proxy_cache_get(policy, &t, &found, bytes, &length));

    /* A GET that was in flight during a successful POST cannot refill the old generation. */
    proxy_cache_invalidate(policy);
    proxy_cache_put(policy, &t, &header, bytes, 1);
    assert(!proxy_cache_get(policy, &t, &found, bytes, &length));
    proxy_policy_free(policy);
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
    test_ketama();
    test_shared_policy();
    test_mime();
    test_proxy_flow();
    puts("Unit tests passed");
    return 0;
}
