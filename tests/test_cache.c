#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "check.h"

#define REPLY_SIZE 1000
/* Two entries with a REPLY_SIZE reply fit. Three do not. */
#define TWO_ENTRY_BYTES 2500

static char ok_reply[REPLY_SIZE];

static struct cache_options options_init(void)
{
    return (struct cache_options){.is_on = true,
                                  .admit = CACHE_ADMIT_ALL,
                                  .evict = CACHE_EVICT_FIFO,
                                  .lock = CACHE_LOCK_GLOBAL,
                                  .bytes = DEFAULT_CACHE_BYTES,
                                  .object_max = DEFAULT_CACHE_OBJECT_MAX,
                                  .ttl = 0};
}

static void ok_reply_init(void)
{
    memset(ok_reply, 'x', sizeof(ok_reply));
    memcpy(ok_reply, "HTTP/1.1 200 OK\r\n\r\n",
           strlen("HTTP/1.1 200 OK\r\n\r\n"));
}

static bool key_is_found(struct cache *cache, const char *key, time_t now)
{
    char *reply = NULL;
    size_t reply_len = 0;
    bool is_found = cache_find(cache, key, now, &reply, &reply_len);
    free(reply);

    return is_found;
}

static void a_stored_reply_is_found_with_the_same_bytes(void)
{
    struct cache_options options = options_init();
    struct cache *cache = cache_create(&options);
    CHECK(cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0));
    char *reply = NULL;
    size_t reply_len = 0;
    CHECK(cache_find(cache, "GET /a", 0, &reply, &reply_len));
    CHECK_EQ_INT(reply_len, 1000);
    CHECK(memcmp(reply, ok_reply, reply_len) == 0);
    free(reply);
    cache_destroy(cache);
}

static void a_key_that_was_not_stored_is_a_miss(void)
{
    struct cache_options options = options_init();
    struct cache *cache = cache_create(&options);
    CHECK(!key_is_found(cache, "GET /a", 0));
    struct cache_stats stats;
    cache_stats_get(cache, &stats);
    CHECK_EQ_INT(stats.misses, 1);
    cache_destroy(cache);
}

static void a_reply_that_is_not_200_is_not_stored(void)
{
    struct cache_options options = options_init();
    struct cache *cache = cache_create(&options);
    const char *reply = "HTTP/1.1 404 Not Found\r\n\r\n";
    CHECK(!cache_store(cache, "GET /a", reply, strlen(reply), 0));
    cache_destroy(cache);
}

static void a_post_is_not_stored(void)
{
    struct cache_options options = options_init();
    struct cache *cache = cache_create(&options);
    CHECK(!cache_store(cache, "POST /a", ok_reply, sizeof(ok_reply), 0));
    cache_destroy(cache);
}

static void size_admission_rejects_a_reply_over_the_object_max(void)
{
    struct cache_options options = options_init();
    options.admit = CACHE_ADMIT_SIZE;
    options.object_max = 999;
    struct cache *cache = cache_create(&options);
    CHECK(!cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0));
    struct cache_stats stats;
    cache_stats_get(cache, &stats);
    CHECK_EQ_INT(stats.rejects, 1);
    cache_destroy(cache);
}

static void second_admission_stores_a_key_on_its_second_miss(void)
{
    struct cache_options options = options_init();
    options.admit = CACHE_ADMIT_SECOND;
    struct cache *cache = cache_create(&options);
    CHECK(!cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0));
    CHECK(!key_is_found(cache, "GET /a", 0));
    CHECK(cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0));
    CHECK(key_is_found(cache, "GET /a", 0));
    cache_destroy(cache);
}

static void a_reply_larger_than_the_budget_is_not_stored(void)
{
    struct cache_options options = options_init();
    options.bytes = 500;
    struct cache *cache = cache_create(&options);
    CHECK(!cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0));
    cache_destroy(cache);
}

static void fifo_evicts_the_oldest_entry_even_after_a_hit(void)
{
    struct cache_options options = options_init();
    options.bytes = TWO_ENTRY_BYTES;
    struct cache *cache = cache_create(&options);
    cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0);
    cache_store(cache, "GET /b", ok_reply, sizeof(ok_reply), 0);
    CHECK(key_is_found(cache, "GET /a", 0));
    cache_store(cache, "GET /c", ok_reply, sizeof(ok_reply), 0);
    CHECK(!key_is_found(cache, "GET /a", 0));
    CHECK(key_is_found(cache, "GET /b", 0));
    CHECK(key_is_found(cache, "GET /c", 0));
    struct cache_stats stats;
    cache_stats_get(cache, &stats);
    CHECK_EQ_INT(stats.evictions, 1);
    cache_destroy(cache);
}

static void lru_evicts_the_least_recently_used_entry(void)
{
    struct cache_options options = options_init();
    options.bytes = TWO_ENTRY_BYTES;
    options.evict = CACHE_EVICT_LRU;
    struct cache *cache = cache_create(&options);
    cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0);
    cache_store(cache, "GET /b", ok_reply, sizeof(ok_reply), 0);
    CHECK(key_is_found(cache, "GET /a", 0));
    cache_store(cache, "GET /c", ok_reply, sizeof(ok_reply), 0);
    CHECK(key_is_found(cache, "GET /a", 0));
    CHECK(!key_is_found(cache, "GET /b", 0));
    CHECK(key_is_found(cache, "GET /c", 0));
    cache_destroy(cache);
}

static void an_entry_expires_when_the_ttl_has_passed(void)
{
    struct cache_options options = options_init();
    options.ttl = 10;
    struct cache *cache = cache_create(&options);
    cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 100);
    CHECK(key_is_found(cache, "GET /a", 109));
    CHECK(!key_is_found(cache, "GET /a", 110));
    struct cache_stats stats;
    cache_stats_get(cache, &stats);
    CHECK_EQ_INT(stats.expirations, 1);
    CHECK_EQ_INT(stats.entries, 0);
    cache_destroy(cache);
}

static void an_entry_does_not_expire_with_ttl_zero(void)
{
    struct cache_options options = options_init();
    struct cache *cache = cache_create(&options);
    cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0);
    CHECK(key_is_found(cache, "GET /a", 1000000));
    cache_destroy(cache);
}

static void striped_lru_evicts_across_stripes(void)
{
    struct cache_options options = options_init();
    options.bytes = TWO_ENTRY_BYTES;
    options.evict = CACHE_EVICT_LRU;
    options.lock = CACHE_LOCK_STRIPED;
    struct cache *cache = cache_create(&options);
    cache_store(cache, "GET /a", ok_reply, sizeof(ok_reply), 0);
    cache_store(cache, "GET /b", ok_reply, sizeof(ok_reply), 0);
    cache_store(cache, "GET /c", ok_reply, sizeof(ok_reply), 0);
    CHECK(!key_is_found(cache, "GET /a", 0));
    CHECK(key_is_found(cache, "GET /c", 0));
    cache_destroy(cache);
}

int main(void)
{
    ok_reply_init();
    a_stored_reply_is_found_with_the_same_bytes();
    a_key_that_was_not_stored_is_a_miss();
    a_reply_that_is_not_200_is_not_stored();
    a_post_is_not_stored();
    size_admission_rejects_a_reply_over_the_object_max();
    second_admission_stores_a_key_on_its_second_miss();
    a_reply_larger_than_the_budget_is_not_stored();
    fifo_evicts_the_oldest_entry_even_after_a_hit();
    lru_evicts_the_least_recently_used_entry();
    an_entry_expires_when_the_ttl_has_passed();
    an_entry_does_not_expire_with_ttl_zero();
    striped_lru_evicts_across_stripes();
    printf("test_cache: ok\n");

    return 0;
}
