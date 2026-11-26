#include "connection.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void set_buffer(int fd, int option)
{
    int size = 16384;
    assert(setsockopt(fd, SOL_SOCKET, option, &size, sizeof size) == 0);
}

static int ready(int fd, short events, int timeout)
{
    struct pollfd p = {.fd = fd, .events = events};
    int result = poll(&p, 1, timeout);
    assert(result >= 0);
    return result;
}

static void step_ready(Connection *c)
{
    struct pollfd p = connection_interest(c);
    assert(ready(p.fd, p.events, 1000) == 1);
    connection_step(c);
}

static void consume_head(int fd)
{
    char head[1024];
    for (size_t n = 0; n < sizeof head; n++) {
        assert(ready(fd, POLLIN, 1000) == 1 && recv(fd, head + n, 1, 0) == 1);
        if (n >= 3 && memcmp(head + n - 3, "\r\n\r\n", 4) == 0)
            return;
    }
    assert(0 && "expected HTTP header");
}

/* Drive the production state machine using real nonblocking sockets. Leave
   the receiver paused until a full sink and readable source coexist. */
static void relay_paused(Connection *c, int source, int sink, int sender, int receiver,
                         const unsigned char *body, size_t length)
{
    size_t supplied = 0, consumed = 0;
    bool paused = false, done = false;
    int64_t end = net_deadline();
    while (!done || consumed < length) {
        assert(net_now_ms() < end);
        if (supplied < length) {
            ssize_t n = send(sender, body + supplied, length - supplied, 0);
            if (n > 0)
                supplied += (size_t)n;
            else
                assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        }
        struct pollfd p = connection_interest(c);
        if ((p.fd == source && p.events == POLLIN) || (p.fd == sink && p.events == POLLOUT)) {
            if (ready(p.fd, p.events, 1)) {
                connection_step(c);
            } else if (!paused && p.fd == sink && ready(source, POLLIN, 0) &&
                       !ready(sink, POLLOUT, 30)) {
                /* Some sockets accept a small final write below the poll
                   low-water mark. Keep filling until a write stays pending. */
                connection_step(c);
                struct pollfd next = connection_interest(c);
                paused = next.fd == sink && next.events == POLLOUT;
            }
        } else {
            done = true;
        }
        if ((paused || done) && consumed < length) {
            unsigned char bytes[16384];
            ssize_t n = recv(receiver, bytes, sizeof bytes, 0);
            if (n > 0) {
                assert((size_t)n <= length - consumed);
                assert(memcmp(bytes, body + consumed, (size_t)n) == 0);
                consumed += (size_t)n;
            } else {
                assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            }
        }
    }
    assert(paused && supplied == length);
}

void test_proxy_flow(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    set_buffer(listener, SO_RCVBUF);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(listener, 1) == 0);
    socklen_t addrlen = sizeof addr;
    assert(getsockname(listener, (struct sockaddr *)&addr, &addrlen) == 0);
    ServerConfig config = {.route_count = 1,
                           .routes = {{.prefix = "/api", .count = 1, .backends = {{.ip = "127.0.0.1"}}}}};
    config.routes[0].backends[0].port = ntohs(addr.sin_port);

    static unsigned char body[512 * 1024];
    for (size_t i = 0; i < sizeof body; i++)
        body[i] = (unsigned char)(i ^ (i >> 8) ^ (i >> 16));

    int client[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
    assert(net_nonblocking(client[0]) == 0 && net_nonblocking(client[1]) == 0);
    set_buffer(client[1], SO_SNDBUF);
    Connection *c = connection_new(client[1], &config);
    assert(c != NULL);
    char head[128];
    int size = snprintf(head, sizeof head, "POST /api HTTP/1.1\r\nHost: local\r\n"
                        "Content-Length: %zu\r\n\r\n", sizeof body);
    assert(send(client[0], head, (size_t)size, 0) == size);
    step_ready(c);
    int upstream = connection_interest(c).fd;
    assert(upstream >= 0 && upstream != client[1] && ready(listener, POLLIN, 1000));
    int backend = accept(listener, NULL, NULL);
    assert(backend >= 0 && net_nonblocking(backend) == 0);
    set_buffer(upstream, SO_SNDBUF);
    step_ready(c);  /* finish connecting */
    step_ready(c);  /* forward request head */
    consume_head(backend);
    relay_paused(c, client[1], upstream, client[0], backend, body, sizeof body);

    size = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
                    sizeof body);
    assert(send(backend, head, (size_t)size, 0) == size);
    step_ready(c);
    step_ready(c);
    consume_head(client[0]);
    relay_paused(c, upstream, client[1], backend, client[0], body, sizeof body);

    assert(connection_interest(c).fd == -1);
    connection_free(c);
    close(backend);
    close(listener);
    close(client[0]);
    close(client[1]);
}
