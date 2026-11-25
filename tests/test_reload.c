/* Unit tests for file routes (-R /p=@file) and route_table_reload / route_live. */
#include "route.h"
#include "test.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static route_table table;             /* route_table is too big for the stack */
static char dir[] = "/tmp/test_reload.XXXXXX";
static char path[PATH_MAX];           /* dir/list: the route file under test */

static void write_file(const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static backend *find(const route *r, const char *host)
{
    for (size_t i = 0; i < r->backend_count; ++i)
        if (strcmp(r->backends[i]->host, host) == 0)
            return r->backends[i];
    return NULL;
}

static void test_file_parse(void)
{
    char spec[PATH_MAX + 32], real[PATH_MAX];
    write_file("# members\n127.0.0.1:1, b:2*3\r\n\t[::1]:3   # trailing comment\n\n");
    route_table_init(&table);
    CHECK(route_table_add(&table, "/i=x:9") == 0);
    snprintf(spec, sizeof spec, "/f@ketama=@%s", path);
    CHECK(route_table_add(&table, spec) == 0);
    CHECK(table.count == 2);
    CHECK_STR(table.routes[0].file, "");
    const route *r = &table.routes[1];
    CHECK_STR(r->prefix, "/f");
    CHECK(r->ketama == 1);
    CHECK(realpath(path, real) != NULL);
    CHECK_STR(r->file, real);
    CHECK(r->backend_count == 3);
    if (r->backend_count == 3) {
        CHECK_STR(r->backends[0]->host, "127.0.0.1");
        CHECK_STR(r->backends[0]->port, "1");
        CHECK_STR(r->backends[1]->host, "b");
        CHECK(r->weight[1] == 3);
        CHECK_STR(r->backends[2]->host, "::1");
        CHECK_STR(r->backends[2]->port, "3");
        CHECK(r->weight[2] == 1);
    }
    CHECK(r->ring_len == ROUTE_RING_POINTS * 5);
    route_table_release(&table);

    /* A relative path is stored as the absolute one. */
    char cwd[PATH_MAX];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK(chdir(dir) == 0);
    route_table_init(&table);
    CHECK(route_table_add(&table, "/rel=@list") == 0);
    CHECK_STR(table.routes[0].file, real);
    CHECK(chdir(cwd) == 0);
    route_table_release(&table);
}

/* Contents every load must reject, at start-up and on reload. */
static const char *bad_files[] = {
    "",                                     /* 0 backends */
    "# only a comment\n\n",
    "a:1 b:2 c:3 d:4 e:5 f:6 g:7 h:8 i:9\n", /* > ROUTE_MAX_BACKENDS */
    "a:1\nb:2\na:1\n",                      /* duplicate host:port */
    "a:1\nnot-a-backend\n",
    "a:1*5\n",
    "a:0\n",
};

static void test_file_invalid_at_start(void)
{
    char spec[PATH_MAX + 32];
    snprintf(spec, sizeof spec, "/f=@%s", path);
    route_table_init(&table);
    CHECK(route_table_add(&table, "/i=x:9") == 0);
    static unsigned char snapshot[sizeof table];
    memcpy(snapshot, &table, sizeof table);
    for (size_t i = 0; i < sizeof bad_files / sizeof bad_files[0]; ++i) {
        write_file(bad_files[i]);
        CHECK(route_table_add(&table, spec) == -1);
        CHECK(memcmp(&table, snapshot, sizeof table) == 0);
    }
    CHECK(route_table_add(&table, "/f=@/nonexistent/route.list") == -1);
    CHECK(memcmp(&table, snapshot, sizeof table) == 0);
    route_table_release(&table);
}

static void test_reload_keeps_state(void)
{
    char spec[PATH_MAX + 32], err[256];
    write_file("a:1\nb:2\n");
    route_table_init(&table);
    CHECK(route_table_add(&table, "/i=x:9*2") == 0);
    snprintf(spec, sizeof spec, "/f@ketama=@%s", path);
    CHECK(route_table_add(&table, spec) == 0);
    route *old = &table.routes[1];
    CHECK(old->backend_count == 2);
    if (old->backend_count != 2) {
        route_table_release(&table);
        return;
    }
    backend *a = old->backends[0], *b = old->backends[1], *x = table.routes[0].backends[0];
    for (int i = 0; i < HEALTH_FAILS_TO_DOWN; ++i)
        backend_report(b, 0);
    CHECK(!backend_is_up(b));
    int64_t changed = b->changed_ms;
    b->req_ok = 5;
    b->req_err = 7;
    b->shed = 3;
    CHECK(backend_try_acquire(b, 4));       /* a request still in flight across the reload */

    write_file("# b stays with a new weight, a goes, c is new\nb:2*2\nc:3\n");
    route_table *next = NULL;
    CHECK(route_table_reload(&table, &next, err, sizeof err) == 0);
    CHECK(next != NULL);
    if (!next) {
        backend_release(b);
        route_table_release(&table);
        return;
    }
    CHECK(next->count == 2);
    CHECK(next->heap == 1);
    CHECK(next->refs == 1);
    const route *r = &next->routes[1];
    CHECK_STR(r->prefix, "/f");
    CHECK_STR(r->file, old->file);
    CHECK(r->ketama == 1);
    CHECK(r->backend_count == 2);
    CHECK(r->backends[0] == b);             /* same object: state carried over */
    CHECK(r->weight[0] == 2);
    CHECK(!backend_is_up(b));
    CHECK(b->changed_ms == changed);
    CHECK(b->req_ok == 5 && b->req_err == 7 && b->shed == 3);
    CHECK(__atomic_load_n(&b->inflight, __ATOMIC_RELAXED) == 1);
    CHECK(find(r, "a") == NULL);
    backend *c = find(r, "c");
    CHECK(c != NULL && c != a);
    if (c) {
        CHECK(backend_is_up(c));
        CHECK(c->req_ok == 0 && c->req_err == 0 && c->fails == 0);
        CHECK(__atomic_load_n(&c->inflight, __ATOMIC_RELAXED) == 0);
    }
    CHECK(r->ring_len == ROUTE_RING_POINTS * 3);
    /* The inline route is shared as is. */
    CHECK(next->routes[0].backends[0] == x);
    CHECK(next->routes[0].weight[0] == 2);
    /* old is untouched until released. */
    CHECK(old->backend_count == 2 && old->backends[0] == a && old->backends[1] == b);

    route_table_release(&table);            /* frees a; b, c, x live on in next */
    CHECK(!backend_is_up(b));
    backend_release(b);                     /* the in-flight request finishes after the swap */
    CHECK(__atomic_load_n(&b->inflight, __ATOMIC_RELAXED) == 0);

    /* Reload of a reload: same pointers again. */
    route_table *third = NULL;
    CHECK(route_table_reload(next, &third, err, sizeof err) == 0);
    if (third) {
        CHECK(third->routes[1].backends[0] == b);
        CHECK(third->routes[1].backends[1] == c);
        route_table_release(third);
    }
    route_table_release(next);
}

static void test_reload_invalid_keeps_old(void)
{
    char spec[PATH_MAX + 32], err[256];
    write_file("a:1,b:2\n");
    route_table_init(&table);
    snprintf(spec, sizeof spec, "/f=@%s", path);
    CHECK(route_table_add(&table, spec) == 0);
    static unsigned char snapshot[sizeof table];
    memcpy(snapshot, &table, sizeof table);
    route_table *sentinel = (route_table *)&snapshot, *out = sentinel;

    for (size_t i = 0; i < sizeof bad_files / sizeof bad_files[0]; ++i) {
        write_file(bad_files[i]);
        err[0] = '\0';
        CHECK(route_table_reload(&table, &out, err, sizeof err) == -1);
        CHECK(out == sentinel);
        CHECK(err[0] != '\0');
        CHECK(memcmp(&table, snapshot, sizeof table) == 0);
    }

    /* Over 64 KiB, even if every byte is a valid token or separator. */
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("a:1\n", f);
        for (int i = 0; i < 64 * 1024; ++i)
            fputc('\n', f);
        fclose(f);
    }
    err[0] = '\0';
    CHECK(route_table_reload(&table, &out, err, sizeof err) == -1);
    CHECK(out == sentinel && err[0] != '\0');
    printf("over 64 KiB: %s\n", err);

    CHECK(unlink(path) == 0);
    err[0] = '\0';
    CHECK(route_table_reload(&table, &out, err, sizeof err) == -1);
    CHECK(out == sentinel && err[0] != '\0');
    printf("missing file: %s\n", err);
    CHECK(memcmp(&table, snapshot, sizeof table) == 0);

    /* The file comes back: reload works again. */
    write_file("b:2\n");
    CHECK(route_table_reload(&table, &out, err, sizeof err) == 0);
    if (out != sentinel) {
        CHECK(out->routes[0].backend_count == 1);
        CHECK(out->routes[0].backends[0] == table.routes[0].backends[1]);
        route_table_release(out);
    }
    route_table_release(&table);
}

/* Workers hold a table and its backends while the main thread swaps tables. ASan/TSan target. */
static route_live live;
static int stop_workers;

static void *worker(void *arg)
{
    (void)arg;
    unsigned long n = 0;
    char key[32];
    while (!__atomic_load_n(&stop_workers, __ATOMIC_ACQUIRE)) {
        route_table *t = route_live_acquire(&live);
        route *r = route_match(t, "/f/x");
        backend *out[ROUTE_MAX_BACKENDS];
        snprintf(key, sizeof key, "10.0.%lu.%lu", (n >> 8) & 255, n & 255);
        size_t got = r ? route_pick(r, key, out, ROUTE_MAX_BACKENDS) : 0;
        for (size_t i = 0; i < got; ++i) {
            if (backend_try_acquire(out[i], 0xffff)) {
                __atomic_add_fetch(&out[i]->req_ok, 1, __ATOMIC_RELAXED);
                backend_release(out[i]);
            }
            (void)strlen(out[i]->host);
        }
        route_table_release(t);
        ++n;
    }
    return NULL;
}

static void test_swap_concurrent(void)
{
    enum { THREADS = 8, SWAPS = 300 };
    static const char *lists[] = { "a:1,b:2,c:3\n", "b:2*2,d:4\n", "e:5,a:1,f:6,g:7,h:8,i:9,j:10,k:11\n" };
    char spec[PATH_MAX + 32], err[256];
    write_file(lists[0]);
    route_table_init(&table);
    snprintf(spec, sizeof spec, "/f@ketama=@%s", path);
    CHECK(route_table_add(&table, spec) == 0);
    route_live_init(&live, &table);

    pthread_t threads[THREADS];
    int started = 0;
    for (; started < THREADS; ++started)
        if (pthread_create(&threads[started], NULL, worker, NULL) != 0)
            break;
    CHECK(started == THREADS);
    int swapped = 0;
    for (int i = 1; i <= SWAPS; ++i) {
        write_file(lists[i % 3]);
        route_table *cur = route_live_acquire(&live), *next = NULL;
        if (route_table_reload(cur, &next, err, sizeof err) == 0) {
            route_live_swap(&live, next);
            ++swapped;
        }
        route_table_release(cur);
    }
    __atomic_store_n(&stop_workers, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < started; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(swapped == SWAPS);

    route_table *t = route_live_acquire(&live);
    CHECK(t->refs == 2);                    /* live + ours: workers released theirs */
    const route *r = &t->routes[0];
    CHECK(r->backend_count == 3);           /* SWAPS % 3 == 0: lists[0] */
    for (size_t i = 0; i < r->backend_count; ++i)
        CHECK(__atomic_load_n(&r->backends[i]->inflight, __ATOMIC_RELAXED) == 0);
    route_table_release(t);
    route_live_destroy(&live);
}

int main(void)
{
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/list", dir);
    test_file_parse();
    test_file_invalid_at_start();
    test_reload_keeps_state();
    test_reload_invalid_keeps_old();
    test_swap_concurrent();
    unlink(path);
    rmdir(dir);
    TEST_MAIN_END();
}
