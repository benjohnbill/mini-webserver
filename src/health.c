#include "health.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "proxy.h"

#define PROBE_INTERVAL_MS 1000
#define PROBE_TIMEOUT_MS  300
#define STOP_POLL_MS      100

static pthread_t g_thread;
static int g_running;
static int g_stop;                    /* __atomic */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int stopping(void) { return __atomic_load_n(&g_stop, __ATOMIC_RELAXED); }

static void *health_main(void *arg)
{
    server_config *cfg = arg;
    /* shortcut: probes run one after another, so N black-holed backends
     * stretch the interval by N x PROBE_TIMEOUT_MS; switch to concurrent
     * non-blocking probes once there are many remote backends. */
    while (!stopping()) {
        int64_t next = now_ms() + PROBE_INTERVAL_MS;
        for (size_t i = 0; i < cfg->routes.count && !stopping(); i++) {
            route *r = &cfg->routes.routes[i];
            for (size_t j = 0; j < r->backend_count && !stopping(); j++) {
                backend *b = &r->backends[j];
                int fd = tcp_connect_timeout(b->host, b->port, PROBE_TIMEOUT_MS);
                if (fd >= 0)
                    close(fd);
                backend_report(b, fd >= 0);
            }
        }
        /* Short sleeps so shutdown never waits a whole interval. */
        while (!stopping() && now_ms() < next) {
            struct timespec ts = { 0, STOP_POLL_MS * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

int health_start(server_config *cfg)
{
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELAXED);
    g_running = pthread_create(&g_thread, NULL, health_main, cfg) == 0;
    return g_running ? 0 : -1;
}

void health_stop(void)
{
    if (!g_running)
        return;
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    pthread_join(g_thread, NULL);
    g_running = 0;
}

static int append(char **p, size_t *left, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *left)
        return -1;
    *p += n;
    *left -= (size_t)n;
    return 0;
}

int lb_status_render(char *out, size_t size, const server_config *cfg,
                     int busy, size_t queued)
{
    char *p = out;
    size_t left = size;
    int64_t now = now_ms();
    if (append(&p, &left, "front :%d  workers %d/%d busy  queue %zu/%d\n",
               cfg->port, busy, cfg->threads, queued, cfg->queue_size) < 0)
        return -1;
    for (size_t i = 0; i < cfg->routes.count; i++) {
        const route *r = &cfg->routes.routes[i];
        if (append(&p, &left, "route %s  strategy %s\n"
                   "  BACKEND          STATE  WEIGHT  SHARE   FAILS  OK    ERR  CHANGED\n",
                   r->prefix, r->ketama ? "ketama" : "round-robin") < 0)
            return -1;
        for (size_t j = 0; j < r->backend_count; j++) {
            const backend *b = &r->backends[j];
            char addr[sizeof b->host + sizeof b->port + 3], share[16], ago[32];
            int v6 = strchr(b->host, ':') != NULL;
            snprintf(addr, sizeof addr, "%s%s%s:%s", v6 ? "[" : "", b->host,
                     v6 ? "]" : "", b->port);
            snprintf(share, sizeof share, "%.1f%%", route_share(r, j) * 100);
            long long s = (now - __atomic_load_n(&b->changed_ms, __ATOMIC_RELAXED)) / 1000;
            if (s < 60)
                snprintf(ago, sizeof ago, "%llds ago", s < 0 ? 0 : s);
            else if (s < 3600)
                snprintf(ago, sizeof ago, "%lldm ago", s / 60);
            else
                snprintf(ago, sizeof ago, "%lldh ago", s / 3600);
            if (append(&p, &left, "  %-16s %-6s %-7u %-7s %-6u %-5lu %-4lu %s\n",
                       addr, backend_is_up(b) ? "UP" : "DOWN", b->weight, share,
                       __atomic_load_n(&b->fails, __ATOMIC_RELAXED),
                       __atomic_load_n(&b->req_ok, __ATOMIC_RELAXED),
                       __atomic_load_n(&b->req_err, __ATOMIC_RELAXED), ago) < 0)
                return -1;
        }
    }
    return (int)(size - left);
}
