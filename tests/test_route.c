#include "route.h"
#include "test.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>

#define KEYS 10000

/* Same hash as route.c, for the naive hash % N comparison. */
static uint32_t hashstr(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (unsigned char)*s) * 16777619u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static const char *key_of(int i)
{
    static char key[32];
    snprintf(key, sizeof key, "10.%d.%d.%d", i >> 16, (i >> 8) & 255, i & 255);
    return key;
}

static size_t pick1(route *r, const char *key)
{
    backend *b = NULL;
    CHECK(route_pick(r, key, &b, 1) == 1);
    return b ? (size_t)(b - r->backends) : 0;
}

static void test_parse(void)
{
    route_table table;
    route_table_init(&table);
    CHECK(table.count == 0);
    CHECK(route_table_add(&table, "/api/=one:1,[::1]:8001,three:65535") == 0);
    CHECK(table.count == 1);
    const route *entry = &table.routes[0];
    CHECK_STR(entry->prefix, "/api/");
    CHECK(entry->backend_count == 3);
    CHECK_STR(entry->backends[0].host, "one");
    CHECK_STR(entry->backends[0].port, "1");
    CHECK_STR(entry->backends[1].host, "::1");
    CHECK_STR(entry->backends[1].port, "8001");
    CHECK_STR(entry->backends[2].port, "65535");
    CHECK(entry->ketama == 0);
    CHECK(entry->backends[0].weight == 1);
    CHECK(entry->ring_len == 0);
    CHECK(backend_is_up(&entry->backends[0]));
    CHECK(entry->backends[0].changed_ms > 0);
    CHECK(__atomic_load_n(&entry->backends[0].inflight, __ATOMIC_RELAXED) == 0);
    CHECK(__atomic_load_n(&entry->backends[0].shed, __ATOMIC_RELAXED) == 0);
    printf("sizeof(route_table) = %zu bytes\n", sizeof table);

    const char *invalid[] = {
        "/api", "/api=host", "/api=host:0", "/api=host:70000",
        "api=host:80", "/api=", "/api=:80", "/api=host:",
        "/api=host:+80", "/api=host:80,", "/api=host:80,,other:81",
        "/api=[::1:8001", "/api=host:00000080",
        "/api=a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9",
        "/api=host:80*0", "/api=host:80*5", "/api=host:80*", "/api=host:80*2x",
        "/api=host:80*12", "/api=host*2:80", "/api=[::1]:80*", "/api@ketama=host:80*9"
    };
    unsigned char snapshot[sizeof table];
    memcpy(snapshot, &table, sizeof table);
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        CHECK(route_table_add(&table, invalid[i]) == -1);
        CHECK(memcmp(&table, snapshot, sizeof table) == 0);
    }

    char long_spec[300];
    memset(long_spec, 'a', 256);
    long_spec[0] = '/';
    strcpy(long_spec + 256, "=host:80");
    CHECK(route_table_add(&table, long_spec) == -1);
    memcpy(long_spec, "/x=", 3);
    memset(long_spec + 3, 'a', 256);
    strcpy(long_spec + 259, ":80");
    CHECK(route_table_add(&table, long_spec) == -1);
    CHECK(memcmp(&table, snapshot, sizeof table) == 0);

    CHECK(route_table_add(&table, "/k@ketama=a:1*2,[::1]:80*4,c:3") == 0);
    entry = &table.routes[1];
    CHECK_STR(entry->prefix, "/k");
    CHECK(entry->ketama == 1);
    CHECK(entry->backends[0].weight == 2);
    CHECK_STR(entry->backends[1].host, "::1");
    CHECK_STR(entry->backends[1].port, "80");
    CHECK(entry->backends[1].weight == 4);
    CHECK(entry->backends[2].weight == 1);
    CHECK(entry->ring_len == ROUTE_RING_POINTS * 7);
    for (size_t j = 1; j < entry->ring_len; ++j)
        CHECK(entry->ring_hash[j - 1] <= entry->ring_hash[j]);

    for (size_t i = 2; i < ROUTE_MAX_ROUTES; ++i)
        CHECK(route_table_add(&table, "/=a:1") == 0);
    CHECK(route_table_add(&table, "/full=a:1") == -1);
    CHECK(table.count == ROUTE_MAX_ROUTES);
}

static void test_match(void)
{
    route_table table;
    route_table_init(&table);
    CHECK(route_table_add(&table, "/api=api:80") == 0);
    CHECK(route_match(&table, "/api") == &table.routes[0]);
    CHECK(route_match(&table, "/api/") == &table.routes[0]);
    CHECK(route_match(&table, "/apix") == NULL);

    CHECK(route_table_add(&table, "/api/=slash:80") == 0);
    CHECK(route_table_add(&table, "/api/v1/=v1:80") == 0);
    CHECK(route_table_add(&table, "/=root:80") == 0);
    CHECK(route_match(&table, "/api") == &table.routes[1]);
    CHECK(route_match(&table, "/api/") == &table.routes[1]);
    CHECK(route_match(&table, "/api/v1") == &table.routes[2]);
    CHECK(route_match(&table, "/api/v1/items") == &table.routes[2]);
    CHECK(route_match(&table, "/api/v10/items") == &table.routes[1]);
    CHECK(route_match(&table, "/apix") == &table.routes[3]);
    CHECK(route_match(&table, "/") == &table.routes[3]);
}

static void test_round_robin(void)
{
    route_table table;
    route_table_init(&table);
    CHECK(route_table_add(&table, "/=a:1,b:2,c:3") == 0);
    CHECK(route_table_add(&table, "/w=a:1*2,b:2,c:3") == 0);
    route *entry = &table.routes[0];
    for (int i = 0; i < 7; ++i)
        CHECK(pick1(entry, NULL) == (size_t)(i % 3));

    /* Weighted 2:1:1 interleaves as a,b,c,a. */
    entry = &table.routes[1];
    size_t counts[3] = {0};
    for (int i = 0; i < 400; ++i)
        ++counts[pick1(entry, NULL)];
    CHECK(counts[0] == 200 && counts[1] == 100 && counts[2] == 100);

    /* Cursor wrap at UINT_MAX must not repeat a slot and skip a backend. */
    entry = &table.routes[0];
    entry->next = UINT_MAX - 1;
    for (int i = 0; i < 4; ++i) {
        backend *out[3];
        CHECK(route_pick(entry, NULL, out, 3) == 3);
        CHECK(out[0] != out[1] && out[1] != out[2] && out[0] != out[2]);
    }
}

static void test_ketama_spread(void)
{
    route_table table;
    route_table_init(&table);
    CHECK(route_table_add(&table, "/@ketama=10.0.0.1:80,10.0.0.2:80,10.0.0.3:80") == 0);
    CHECK(route_table_add(&table, "/w@ketama=10.0.0.1:80*2,10.0.0.2:80,10.0.0.3:80") == 0);
    route *r = &table.routes[0];

    static size_t before[KEYS];
    size_t counts[3] = {0};
    for (int i = 0; i < KEYS; ++i)
        ++counts[before[i] = pick1(r, key_of(i))];
    for (size_t b = 0; b < 3; ++b) {
        printf("ketama equal: backend %zu %.1f%% of keys (ring share %.1f%%)\n", b,
               100.0 * counts[b] / KEYS, 100.0 * route_share(r, b));
        CHECK(counts[b] > KEYS * 28 / 100 && counts[b] < KEYS * 38 / 100);
    }
    CHECK(pick1(r, NULL) == pick1(r, ""));

    for (int i = 0; i < 3; ++i)
        backend_report(&r->backends[1], 0);
    CHECK(!backend_is_up(&r->backends[1]));
    int moved = 0, naive_moved = 0;
    for (int i = 0; i < KEYS; ++i) {
        size_t now = pick1(r, key_of(i));
        if (before[i] == 1)
            CHECK(now != 1);
        else
            CHECK(now == before[i]);
        moved += now != before[i];
        uint32_t h = hashstr(key_of(i));
        naive_moved += h % 3 != h % 2;
    }
    printf("backend 1 down: ketama moved %.1f%% of keys, naive hash %% N moved %.1f%%\n",
           100.0 * moved / KEYS, 100.0 * naive_moved / KEYS);

    r = &table.routes[1];
    size_t heavy = 0;
    for (int i = 0; i < KEYS; ++i)
        heavy += pick1(r, key_of(i)) == 0;
    printf("ketama 2:1:1: backend 0 %.1f%% of keys\n", 100.0 * heavy / KEYS);
    CHECK(heavy > KEYS * 45 / 100 && heavy < KEYS * 55 / 100);
}

static void test_pick(void)
{
    route_table table;
    route_table_init(&table);
    CHECK(route_table_add(&table, "/@ketama=a:1,b:2,c:3") == 0);
    CHECK(route_table_add(&table, "/rr=a:1*3,b:2,c:3*2") == 0);
    for (size_t t = 0; t < 2; ++t) {
        route *r = &table.routes[t];
        backend *out[4];
        CHECK(route_pick(r, "k", out, 0) == 0);
        CHECK(route_pick(r, "k", out, 2) == 2);
        CHECK(out[0] != out[1]);
        CHECK(route_pick(r, "k", out, 4) == 3);
        CHECK(out[0] != out[1] && out[1] != out[2] && out[0] != out[2]);

        for (int i = 0; i < 3; ++i)
            backend_report(&r->backends[0], 0);
        CHECK(route_pick(r, "k", out, 4) == 2);
        CHECK(out[0] != &r->backends[0] && out[1] != &r->backends[0]);
        for (size_t b = 1; b < 3; ++b)
            for (int i = 0; i < 3; ++i)
                backend_report(&r->backends[b], 0);
        CHECK(route_pick(r, "k", out, 4) == 0);
    }
}

static void test_health(void)
{
    backend b = {0};
    backend_report(&b, 0);
    backend_report(&b, 0);
    CHECK(backend_is_up(&b));
    backend_report(&b, 1);             /* resets the fail streak */
    backend_report(&b, 0);
    backend_report(&b, 0);
    CHECK(backend_is_up(&b));
    backend_report(&b, 0);
    CHECK(!backend_is_up(&b));
    CHECK(b.changed_ms > 0);
    int64_t down_at = b.changed_ms;
    backend_report(&b, 0);             /* already DOWN: no second transition */
    CHECK(b.changed_ms == down_at);
    backend_report(&b, 1);
    CHECK(!backend_is_up(&b));
    backend_report(&b, 0);             /* resets the ok streak */
    backend_report(&b, 1);
    CHECK(!backend_is_up(&b));
    backend_report(&b, 1);
    CHECK(backend_is_up(&b));
    CHECK(b.req_ok == 0 && b.req_err == 0);
}

static void test_share(void)
{
    route_table table;
    route_table_init(&table);
    CHECK(route_table_add(&table, "/@ketama=a:1*2,b:2,c:3*3") == 0);
    CHECK(route_table_add(&table, "/rr=a:1*2,b:2,c:3*3") == 0);
    for (size_t t = 0; t < 2; ++t) {
        double sum = 0;
        for (size_t b = 0; b < 3; ++b)
            sum += route_share(&table.routes[t], b);
        CHECK(sum > 0.999999 && sum < 1.000001);
    }
    CHECK(route_share(&table.routes[1], 2) == 0.5);
}

static void test_backend_cap(void)
{
    backend b = {0};
    CHECK(!backend_try_acquire(&b, 0));
    CHECK(backend_try_acquire(&b, 2));
    CHECK(backend_try_acquire(&b, 2));
    CHECK(!backend_try_acquire(&b, 2));
    CHECK(__atomic_load_n(&b.inflight, __ATOMIC_RELAXED) == 2);
    backend_release(&b);
    CHECK(backend_try_acquire(&b, 2));
    backend_release(&b);
    backend_release(&b);
    CHECK(__atomic_load_n(&b.inflight, __ATOMIC_RELAXED) == 0);
    CHECK(__atomic_load_n(&b.shed, __ATOMIC_RELAXED) == 0);
    CHECK(backend_is_up(&b));
}

typedef struct {
    backend *b;
    unsigned cap;
    unsigned max;
} cap_worker;

static void *exercise_backend_cap(void *arg)
{
    cap_worker *worker = arg;
    for (int i = 0; i < 100000; ++i) {
        if (backend_try_acquire(worker->b, worker->cap)) {
            unsigned inflight = __atomic_load_n(&worker->b->inflight, __ATOMIC_RELAXED);
            if (inflight > worker->max)
                worker->max = inflight;
            backend_release(worker->b);
        }
    }
    return NULL;
}

static void test_backend_cap_concurrent(void)
{
    enum { THREADS = 8, CAP = 2 };
    backend b = {0};
    pthread_t threads[THREADS];
    cap_worker workers[THREADS];
    int started = 0;
    for (; started < THREADS; ++started) {
        workers[started] = (cap_worker){.b = &b, .cap = CAP};
        int rc = pthread_create(&threads[started], NULL, exercise_backend_cap, &workers[started]);
        CHECK(rc == 0);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < started; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(workers[i].max <= CAP);
    }
    CHECK(__atomic_load_n(&b.inflight, __ATOMIC_RELAXED) == 0);
}

int main(void)
{
    test_parse();
    test_match();
    test_round_robin();
    test_ketama_spread();
    test_pick();
    test_health();
    test_share();
    test_backend_cap();
    test_backend_cap_concurrent();
    TEST_MAIN_END();
}
