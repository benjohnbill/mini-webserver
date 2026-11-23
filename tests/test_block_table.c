#include <stdio.h>
#include <string.h>

#include "block_table.h"
#include "check.h"

static struct block_table *blocks_make(size_t key_max)
{
    struct block_table *blocks = block_table_create(key_max);
    CHECK(blocks != NULL);

    return blocks;
}

static void a_key_is_blocked_until_its_time(void)
{
    struct block_table *blocks = blocks_make(4);
    CHECK(block_table_add(blocks, "10.0.0.1", 60, 0));
    CHECK(block_table_has_key(blocks, "10.0.0.1", 0));
    CHECK(block_table_has_key(blocks, "10.0.0.1", 59.5));
    CHECK(!block_table_has_key(blocks, "10.0.0.1", 60));
    block_table_destroy(blocks);
}

static void a_key_that_was_never_added_is_not_blocked(void)
{
    struct block_table *blocks = blocks_make(4);
    CHECK(block_table_add(blocks, "10.0.0.1", 60, 0));
    CHECK(!block_table_has_key(blocks, "10.0.0.2", 0));
    block_table_destroy(blocks);
}

static void a_second_add_keeps_the_later_time(void)
{
    struct block_table *blocks = blocks_make(4);
    CHECK(block_table_add(blocks, "a", 100, 0));
    CHECK(block_table_add(blocks, "a", 50, 10));
    CHECK(block_table_has_key(blocks, "a", 99));
    CHECK(block_table_add(blocks, "a", 200, 20));
    CHECK(block_table_has_key(blocks, "a", 199));
    block_table_destroy(blocks);
}

static void an_expired_key_can_be_blocked_again(void)
{
    struct block_table *blocks = blocks_make(4);
    CHECK(block_table_add(blocks, "a", 10, 0));
    CHECK(!block_table_has_key(blocks, "a", 20));
    CHECK(block_table_add(blocks, "a", 30, 20));
    CHECK(block_table_has_key(blocks, "a", 25));
    block_table_destroy(blocks);
}

static void a_full_table_forgets_an_expired_key_first(void)
{
    struct block_table *blocks = blocks_make(2);
    CHECK(block_table_add(blocks, "short", 10, 0));
    CHECK(block_table_add(blocks, "long", 100, 0));
    CHECK(block_table_add(blocks, "new", 100, 50));
    CHECK(block_table_has_key(blocks, "long", 50));
    CHECK(block_table_has_key(blocks, "new", 50));
    block_table_destroy(blocks);
}

static void
a_full_table_with_no_expired_key_forgets_the_key_that_expires_first(void)
{
    struct block_table *blocks = blocks_make(2);
    CHECK(block_table_add(blocks, "late", 300, 0));
    CHECK(block_table_add(blocks, "soon", 100, 0));
    CHECK(block_table_add(blocks, "new", 200, 0));
    CHECK(!block_table_has_key(blocks, "soon", 1));
    CHECK(block_table_has_key(blocks, "late", 1));
    CHECK(block_table_has_key(blocks, "new", 1));
    block_table_destroy(blocks);
}

static void an_empty_key_is_refused(void)
{
    struct block_table *blocks = blocks_make(4);
    CHECK(!block_table_add(blocks, "", 60, 0));
    CHECK(!block_table_has_key(blocks, "", 0));
    block_table_destroy(blocks);
}

static void a_key_over_the_limit_is_refused(void)
{
    char key[BLOCK_KEY_MAX + 2];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    struct block_table *blocks = blocks_make(4);
    CHECK(!block_table_add(blocks, key, 60, 0));
    key[BLOCK_KEY_MAX] = '\0';
    CHECK(block_table_add(blocks, key, 60, 0));
    CHECK(block_table_has_key(blocks, key, 0));
    block_table_destroy(blocks);
}

static void an_ipv6_address_and_a_ja4_fit_as_keys(void)
{
    struct block_table *blocks = blocks_make(4);
    CHECK(block_table_add(
        blocks, "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255", 60, 0));
    CHECK(
        block_table_add(blocks, "t13d1516h2_8daaf6152771_e5627efa2ab1", 60, 0));
    CHECK(
        block_table_has_key(blocks, "t13d1516h2_8daaf6152771_e5627efa2ab1", 0));
    block_table_destroy(blocks);
}

static void a_table_of_zero_keys_is_not_made(void)
{
    CHECK(block_table_create(0) == NULL);
}

int main(void)
{
    a_key_is_blocked_until_its_time();
    a_key_that_was_never_added_is_not_blocked();
    a_second_add_keeps_the_later_time();
    an_expired_key_can_be_blocked_again();
    a_full_table_forgets_an_expired_key_first();
    a_full_table_with_no_expired_key_forgets_the_key_that_expires_first();
    an_empty_key_is_refused();
    a_key_over_the_limit_is_refused();
    an_ipv6_address_and_a_ja4_fit_as_keys();
    a_table_of_zero_keys_is_not_made();
    printf("test_block_table: ok\n");

    return 0;
}
