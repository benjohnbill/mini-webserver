#include <stdio.h>
#include <string.h>

#include "behavior.h"
#include "check.h"

#define WINDOW 600.0

static struct behavior *counters_make(size_t key_max)
{
    struct behavior_options options = {.window = WINDOW, .key_max = key_max};
    struct behavior *counters = behavior_create(&options);
    CHECK(counters != NULL);

    return counters;
}

/* Adds one request and returns the counts after it. */
static struct behavior_counts request_add(struct behavior *counters,
                                          const char *key, const char *target,
                                          double now)
{
    struct behavior_counts counts;
    CHECK(behavior_add_request(counters, key, target, now, &counts));

    return counts;
}

static void a_first_request_counts_one_request_and_one_target(void)
{
    struct behavior *counters = counters_make(4);
    struct behavior_counts counts = request_add(counters, "10.0.0.1", "/", 0);
    CHECK_EQ_INT(counts.requests, 1);
    CHECK_EQ_INT(counts.targets, 1);
    CHECK_EQ_INT(counts.not_found, 0);
    CHECK_EQ_INT(counts.robots_reads, 0);
    CHECK_EQ_INT(counts.trap_hits, 0);
    behavior_destroy(counters);
}

static void the_same_target_again_is_not_a_new_target(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/index.html", 0);
    struct behavior_counts counts =
        request_add(counters, "a", "/index.html", 1);
    CHECK_EQ_INT(counts.requests, 2);
    CHECK_EQ_INT(counts.targets, 1);
    behavior_destroy(counters);
}

static void different_targets_are_counted_once_each(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/", 0);
    request_add(counters, "a", "/style.css", 0);
    request_add(counters, "a", "/app.js", 0);
    request_add(counters, "a", "/style.css", 0);
    struct behavior_counts counts = request_add(counters, "a", "/image.png", 0);
    CHECK_EQ_INT(counts.requests, 5);
    CHECK_EQ_INT(counts.targets, 4);
    behavior_destroy(counters);
}

static void a_query_makes_a_different_target(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/search?q=1", 0);
    struct behavior_counts counts =
        request_add(counters, "a", "/search?q=2", 0);
    CHECK_EQ_INT(counts.targets, 2);
    behavior_destroy(counters);
}

static void the_distinct_target_count_stops_at_the_size_of_the_set(void)
{
    struct behavior *counters = counters_make(4);
    char target[32];
    struct behavior_counts counts = {0};
    for (int i = 0; i < 2000; i++) {
        snprintf(target, sizeof(target), "/bench/f%04d.txt", i);
        counts = request_add(counters, "a", target, 0);
    }
    CHECK_EQ_INT(counts.requests, 2000);
    CHECK(counts.targets <= 256);
    CHECK(counts.targets > 200);
    behavior_destroy(counters);
}

static void a_robots_txt_read_is_counted_with_or_without_a_query(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/robots.txt", 0);
    request_add(counters, "a", "/robots.txt?x=1", 0);
    request_add(counters, "a", "/robots.txt.bak", 0);
    struct behavior_counts counts = request_add(counters, "a", "/robots", 0);
    CHECK_EQ_INT(counts.robots_reads, 2);
    behavior_destroy(counters);
}

static void a_target_under_the_honeypot_is_a_trap_hit(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/trap/", 0);
    request_add(counters, "a", "/trap/deep/page.html", 0);
    request_add(counters, "a", "/trap/?x=1", 0);
    request_add(counters, "a", "/trap", 0);
    struct behavior_counts counts = request_add(counters, "a", "/traps/", 0);
    CHECK_EQ_INT(counts.trap_hits, 3);
    behavior_destroy(counters);
}

static void a_404_reply_is_counted_and_other_replies_are_not(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/missing", 0);
    CHECK(behavior_add_reply(counters, "a", 404, 0));
    request_add(counters, "a", "/", 0);
    CHECK(behavior_add_reply(counters, "a", 200, 0));
    request_add(counters, "a", "/x", 0);
    CHECK(behavior_add_reply(counters, "a", 403, 0));
    request_add(counters, "a", "/gone", 0);
    CHECK(behavior_add_reply(counters, "a", 404, 0));
    struct behavior_counts counts = request_add(counters, "a", "/", 1);
    CHECK_EQ_INT(counts.not_found, 2);
    behavior_destroy(counters);
}

static void a_429_reply_is_counted_as_slowed(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/", 0);
    CHECK(behavior_add_reply(counters, "a", 429, 0));
    request_add(counters, "a", "/", 0);
    CHECK(behavior_add_reply(counters, "a", 200, 0));
    request_add(counters, "a", "/", 0);
    CHECK(behavior_add_reply(counters, "a", 429, 0));
    struct behavior_counts counts = request_add(counters, "a", "/", 1);
    CHECK_EQ_INT(counts.slowed, 2);
    CHECK_EQ_INT(counts.not_found, 0);
    behavior_destroy(counters);
}

static void a_reply_for_a_key_with_no_request_is_refused(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/", 0);
    CHECK(!behavior_add_reply(counters, "b", 404, 0));
    behavior_destroy(counters);
}

static void the_counts_start_again_when_the_window_ends(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/robots.txt", 0);
    CHECK(behavior_add_reply(counters, "a", 404, 0));
    struct behavior_counts counts = request_add(counters, "a", "/", 599.5);
    CHECK_EQ_INT(counts.requests, 2);
    CHECK_EQ_INT(counts.not_found, 1);
    counts = request_add(counters, "a", "/", 600);
    CHECK_EQ_INT(counts.requests, 1);
    CHECK_EQ_INT(counts.targets, 1);
    CHECK_EQ_INT(counts.not_found, 0);
    CHECK_EQ_INT(counts.robots_reads, 0);
    behavior_destroy(counters);
}

static void a_reply_after_the_window_ended_is_refused(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "a", "/missing", 0);
    CHECK(!behavior_add_reply(counters, "a", 404, 600));
    struct behavior_counts counts = request_add(counters, "a", "/", 600);
    CHECK_EQ_INT(counts.not_found, 0);
    behavior_destroy(counters);
}

static void keys_are_counted_apart(void)
{
    struct behavior *counters = counters_make(4);
    request_add(counters, "10.0.0.1", "/trap/", 0);
    request_add(counters, "10.0.0.1", "/a", 0);
    struct behavior_counts counts = request_add(counters, "10.0.0.2", "/", 0);
    CHECK_EQ_INT(counts.requests, 1);
    CHECK_EQ_INT(counts.trap_hits, 0);
    behavior_destroy(counters);
}

static void a_full_table_forgets_the_key_used_longest_ago(void)
{
    struct behavior *counters = counters_make(2);
    request_add(counters, "old", "/", 0);
    request_add(counters, "recent", "/", 1);
    request_add(counters, "old", "/", 2);
    request_add(counters, "new", "/", 3);
    struct behavior_counts counts = request_add(counters, "old", "/", 4);
    CHECK_EQ_INT(counts.requests, 3);
    counts = request_add(counters, "recent", "/", 5);
    CHECK_EQ_INT(counts.requests, 1);
    behavior_destroy(counters);
}

static void an_empty_key_is_refused(void)
{
    struct behavior *counters = counters_make(4);
    struct behavior_counts counts;
    CHECK(!behavior_add_request(counters, "", "/", 0, &counts));
    CHECK(!behavior_add_reply(counters, "", 404, 0));
    behavior_destroy(counters);
}

static void a_key_over_the_limit_is_refused(void)
{
    char key[BEHAVIOR_KEY_MAX + 2];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    struct behavior *counters = counters_make(4);
    struct behavior_counts counts;
    CHECK(!behavior_add_request(counters, key, "/", 0, &counts));
    key[BEHAVIOR_KEY_MAX] = '\0';
    CHECK(behavior_add_request(counters, key, "/", 0, &counts));
    CHECK_EQ_INT(counts.requests, 1);
    behavior_destroy(counters);
}

static void a_table_of_zero_keys_is_not_made(void)
{
    struct behavior_options options = {.window = WINDOW, .key_max = 0};
    CHECK(behavior_create(&options) == NULL);
}

int main(void)
{
    a_first_request_counts_one_request_and_one_target();
    the_same_target_again_is_not_a_new_target();
    different_targets_are_counted_once_each();
    a_query_makes_a_different_target();
    the_distinct_target_count_stops_at_the_size_of_the_set();
    a_robots_txt_read_is_counted_with_or_without_a_query();
    a_target_under_the_honeypot_is_a_trap_hit();
    a_404_reply_is_counted_and_other_replies_are_not();
    a_429_reply_is_counted_as_slowed();
    a_reply_for_a_key_with_no_request_is_refused();
    the_counts_start_again_when_the_window_ends();
    a_reply_after_the_window_ended_is_refused();
    keys_are_counted_apart();
    a_full_table_forgets_the_key_used_longest_ago();
    an_empty_key_is_refused();
    a_key_over_the_limit_is_refused();
    a_table_of_zero_keys_is_not_made();
    printf("test_behavior: ok\n");

    return 0;
}
