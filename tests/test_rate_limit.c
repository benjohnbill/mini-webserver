#include <stdio.h>

#include "check.h"
#include "rate_limit.h"

static struct rate_limit *limit_make(double rate, double burst, size_t key_max)
{
    struct rate_limit_options options = {rate, burst, key_max};
    struct rate_limit *limit = rate_limit_create(&options);
    CHECK(limit != NULL);

    return limit;
}

static void a_new_key_can_use_its_whole_burst_at_once(void)
{
    struct rate_limit *limit = limit_make(1, 3, 16);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK(rate_limit_take(limit, "a", 0) > 0);
    rate_limit_destroy(limit);
}

static void a_token_comes_back_after_one_over_rate_seconds(void)
{
    struct rate_limit *limit = limit_make(2, 1, 16);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK(rate_limit_take(limit, "a", 0.1) > 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0.5), 0);
    rate_limit_destroy(limit);
}

static void retry_after_is_the_whole_seconds_to_the_next_token(void)
{
    /* 0.25 is exact in binary, so no rounding hides in the numbers. */
    struct rate_limit *limit = limit_make(0.25, 1, 16);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 1), 3);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 3.5), 1);
    rate_limit_destroy(limit);
}

static void a_refused_request_takes_no_token(void)
{
    struct rate_limit *limit = limit_make(1, 1, 16);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK(rate_limit_take(limit, "a", 0.5) > 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 1.0), 0);
    rate_limit_destroy(limit);
}

static void tokens_do_not_grow_over_the_burst(void)
{
    struct rate_limit *limit = limit_make(1, 2, 16);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 100), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 100), 0);
    CHECK(rate_limit_take(limit, "a", 100) > 0);
    rate_limit_destroy(limit);
}

static void keys_do_not_share_tokens(void)
{
    struct rate_limit *limit = limit_make(1, 1, 16);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK(rate_limit_take(limit, "a", 0) > 0);
    CHECK_EQ_INT(rate_limit_take(limit, "b", 0), 0);
    rate_limit_destroy(limit);
}

static void a_full_table_forgets_the_key_used_longest_ago(void)
{
    struct rate_limit *limit = limit_make(0.01, 1, 2);
    CHECK_EQ_INT(rate_limit_take(limit, "a", 0), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "b", 1), 0);
    CHECK_EQ_INT(rate_limit_take(limit, "c", 2), 0);
    /* "a" was forgotten, so it starts again with a full bucket. */
    CHECK_EQ_INT(rate_limit_take(limit, "a", 3), 0);
    /* "c" is still in the table, with an empty bucket. */
    CHECK(rate_limit_take(limit, "c", 3) > 0);
    rate_limit_destroy(limit);
}

int main(void)
{
    a_new_key_can_use_its_whole_burst_at_once();
    a_token_comes_back_after_one_over_rate_seconds();
    retry_after_is_the_whole_seconds_to_the_next_token();
    a_refused_request_takes_no_token();
    tokens_do_not_grow_over_the_burst();
    keys_do_not_share_tokens();
    a_full_table_forgets_the_key_used_longest_ago();
    printf("test_rate_limit: ok\n");

    return 0;
}
