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
        route_table *table = route_live_acquire(&cfg->live);
        for (size_t i = 0; i < table->count && !stopping(); i++) {
            const route *r = &table->routes[i];
            for (size_t j = 0; j < r->backend_count && !stopping(); j++) {
                backend *b = r->backends[j];
                int fd = tcp_connect_timeout(b->host, b->port, PROBE_TIMEOUT_MS);
                if (fd >= 0)
                    close(fd);
                backend_report(b, fd >= 0);
            }
        }
        route_table_release(table);
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
                     int busy, size_t queued,
                     unsigned long shed_queue, unsigned long shed_stale)
{
    enum { NCOL = 10 };
    static const char *const head[NCOL] = { "BACKEND", "STATE", "WEIGHT", "SHARE",
        "INFLIGHT", "SHED", "FAILS", "OK", "ERR", "CHANGED" };
    /* Minimum widths keep the usual table looking the same; longer values
     * (IPv6 hosts, big counters) widen their column for the whole route. */
    static const int min_w[NCOL] = { 16, 6, 7, 7, 9, 5, 6, 4, 4, 0 };
    char cell[ROUTE_MAX_BACKENDS][NCOL][sizeof ((backend *)0)->host + 16];
    char *p = out;
    size_t left = size;
    int64_t now = now_ms();
    if (append(&p, &left, "front :%d  workers %d/%d busy  queue %zu/%d  "
               "shed %lu (queue) %lu (stale)\n",
               cfg->port, busy, cfg->threads, queued, cfg->queue_size,
               shed_queue, shed_stale) < 0)
        return -1;
    /* Taking a snapshot only changes live's lock and the table's refcount. */
    route_table *table = route_live_acquire((route_live *)&cfg->live);
    for (size_t i = 0; i < table->count; i++) {
        const route *r = &table->routes[i];
        int w[NCOL];
        for (int c = 0; c < NCOL; c++)
            w[c] = min_w[c] > (int)strlen(head[c]) ? min_w[c] : (int)strlen(head[c]);
        /* Pass 1: snapshot every value as text and widen columns to fit. */
        for (size_t j = 0; j < r->backend_count; j++) {
            const backend *b = r->backends[j];
            char (*v)[sizeof cell[0][0]] = cell[j];
            size_t vs = sizeof cell[0][0];
            int v6 = strchr(b->host, ':') != NULL;
            snprintf(v[0], vs, "%s%s%s:%s", v6 ? "[" : "", b->host,
                     v6 ? "]" : "", b->port);
            snprintf(v[1], vs, "%s", backend_is_up(b) ? "UP" : "DOWN");
            snprintf(v[2], vs, "%u", r->weight[j]);
            snprintf(v[3], vs, "%.1f%%", route_share(r, j) * 100);
            unsigned inf = __atomic_load_n(&b->inflight, __ATOMIC_RELAXED);
            if (cfg->backend_cap == 0)
                snprintf(v[4], vs, "%u/-", inf);
            else
                snprintf(v[4], vs, "%u/%u%s", inf, cfg->backend_cap,
                         inf >= cfg->backend_cap ? " FULL" : "");
            snprintf(v[5], vs, "%lu", __atomic_load_n(&b->shed, __ATOMIC_RELAXED));
            snprintf(v[6], vs, "%u", __atomic_load_n(&b->fails, __ATOMIC_RELAXED));
            snprintf(v[7], vs, "%lu", __atomic_load_n(&b->req_ok, __ATOMIC_RELAXED));
            snprintf(v[8], vs, "%lu", __atomic_load_n(&b->req_err, __ATOMIC_RELAXED));
            long long s = (now - __atomic_load_n(&b->changed_ms, __ATOMIC_RELAXED)) / 1000;
            if (s < 60)
                snprintf(v[9], vs, "%llds ago", s < 0 ? 0 : s);
            else if (s < 3600)
                snprintf(v[9], vs, "%lldm ago", s / 60);
            else
                snprintf(v[9], vs, "%lldh ago", s / 3600);
            for (int c = 0; c < NCOL; c++)
                if ((int)strlen(v[c]) > w[c])
                    w[c] = (int)strlen(v[c]);
        }
        char capv[16] = "-";
        if (cfg->backend_cap)
            snprintf(capv, sizeof capv, "%u", cfg->backend_cap);
        if (append(&p, &left, "route %s  strategy %s  inflight cap %s\n",
                   r->prefix, r->ketama ? "ketama" : "round-robin", capv) < 0)
            goto too_small;
        /* Pass 2: header (row -1) and rows share the widths; last column unpadded. */
        for (long j = -1; j < (long)r->backend_count; j++) {
            if (append(&p, &left, " ") < 0)
                goto too_small;
            for (int c = 0; c < NCOL; c++)
                if (append(&p, &left, " %-*s", c == NCOL - 1 ? 0 : w[c],
                           j < 0 ? head[c] : cell[j][c]) < 0)
                    goto too_small;
            if (append(&p, &left, "\n") < 0)
                goto too_small;
        }
    }
    route_table_release(table);
    return (int)(size - left);

too_small:
    route_table_release(table);
    return -1;
}
