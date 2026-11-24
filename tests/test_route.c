#include "route.h"
#include "test.h"

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

    const char *invalid[] = {
        "/api", "/api=host", "/api=host:0", "/api=host:70000",
        "api=host:80", "/api=", "/api=:80", "/api=host:",
        "/api=host:+80", "/api=host:80,", "/api=host:80,,other:81",
        "/api=[::1:8001", "/api=host:00000080",
        "/api=a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9"
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

    for (size_t i = 1; i < ROUTE_MAX_ROUTES; ++i)
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
    route *entry = &table.routes[0];
    for (size_t i = 0; i < 7; ++i)
        CHECK(route_next_backend(entry) == &entry->backends[i % 3]);
}

int main(void)
{
    test_parse();
    test_match();
    test_round_robin();
    TEST_MAIN_END();
}
