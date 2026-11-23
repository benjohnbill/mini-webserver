#include <stdio.h>

#include "check.h"
#include "ip_list.h"

static struct ip_list block_list;

static void a_cidr_range_matches_the_addresses_inside_it(void)
{
    CHECK_EQ_INT(ip_list_parse("10.1.0.0/16\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "10.1.0.1"));
    CHECK(ip_list_has_address(&block_list, "10.1.255.255"));
    CHECK(!ip_list_has_address(&block_list, "10.2.0.1"));
}

static void a_bare_address_matches_only_itself(void)
{
    CHECK_EQ_INT(ip_list_parse("127.0.0.7\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "127.0.0.7"));
    CHECK(!ip_list_has_address(&block_list, "127.0.0.8"));
}

static void a_prefix_that_is_not_on_a_byte_boundary_matches_by_bits(void)
{
    CHECK_EQ_INT(ip_list_parse("192.168.1.64/26", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "192.168.1.64"));
    CHECK(ip_list_has_address(&block_list, "192.168.1.127"));
    CHECK(!ip_list_has_address(&block_list, "192.168.1.63"));
    CHECK(!ip_list_has_address(&block_list, "192.168.1.128"));
}

static void bits_after_the_prefix_in_the_address_are_ignored(void)
{
    CHECK_EQ_INT(ip_list_parse("10.9.9.9/8\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "10.0.0.1"));
}

static void a_zero_bit_prefix_matches_every_address_of_its_family(void)
{
    CHECK_EQ_INT(ip_list_parse("0.0.0.0/0\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "203.0.113.5"));
    CHECK(!ip_list_has_address(&block_list, "2001:db8::1"));
}

static void an_ipv6_range_matches_ipv6_addresses(void)
{
    CHECK_EQ_INT(ip_list_parse("2001:db8::/32\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "2001:db8:ffff::1"));
    CHECK(!ip_list_has_address(&block_list, "2001:db9::1"));
    CHECK(!ip_list_has_address(&block_list, "32.1.13.184"));
}

static void an_ipv4_mapped_ipv6_address_matches_an_ipv4_range(void)
{
    CHECK_EQ_INT(ip_list_parse("127.0.0.0/8\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "::ffff:127.0.0.9"));
}

static void comments_and_blank_lines_are_skipped(void)
{
    const char *text = "# crawlers\n"
                       "\n"
                       "   \n"
                       "10.0.0.0/8   # a data center\n"
                       "\t172.16.0.0/12\n";
    CHECK_EQ_INT(ip_list_parse(text, &block_list), 0);
    CHECK_EQ_INT(block_list.count, 2);
    CHECK(ip_list_has_address(&block_list, "172.20.0.1"));
}

static void crlf_line_ends_are_accepted(void)
{
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/8\r\n11.0.0.0/8\r\n", &block_list), 0);
    CHECK(ip_list_has_address(&block_list, "11.0.0.1"));
}

static void an_empty_text_gives_an_empty_list(void)
{
    CHECK_EQ_INT(ip_list_parse("", &block_list), 0);
    CHECK_EQ_INT(block_list.count, 0);
    CHECK(!ip_list_has_address(&block_list, "10.0.0.1"));
}

static void a_bad_address_gives_its_line_number(void)
{
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/8\n# ok\n10.0.0/8\n", &block_list), 3);
}

static void a_prefix_longer_than_the_address_gives_its_line_number(void)
{
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/33\n", &block_list), 1);
    CHECK_EQ_INT(ip_list_parse("::/129\n", &block_list), 1);
}

static void a_prefix_that_is_not_a_number_gives_its_line_number(void)
{
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/\n", &block_list), 1);
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/8x\n", &block_list), 1);
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/-1\n", &block_list), 1);
}

static void two_ranges_on_one_line_give_its_line_number(void)
{
    CHECK_EQ_INT(ip_list_parse("10.0.0.0/8 11.0.0.0/8\n", &block_list), 1);
}

static void a_line_after_the_last_free_range_gives_its_line_number(void)
{
    static char text[(IP_LIST_RANGE_MAX + 1) * 16];
    size_t used = 0;
    for (int i = 0; i <= IP_LIST_RANGE_MAX; i++) {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "10.%d.%d.0/24\n", i / 256, i % 256);
    }
    CHECK_EQ_INT(ip_list_parse(text, &block_list), 1025);
}

static void text_that_is_not_an_address_matches_nothing(void)
{
    CHECK_EQ_INT(ip_list_parse("0.0.0.0/0\n::/0\n", &block_list), 0);
    CHECK(!ip_list_has_address(&block_list, "-"));
    CHECK(!ip_list_has_address(&block_list, ""));
}

int main(void)
{
    a_cidr_range_matches_the_addresses_inside_it();
    a_bare_address_matches_only_itself();
    a_prefix_that_is_not_on_a_byte_boundary_matches_by_bits();
    bits_after_the_prefix_in_the_address_are_ignored();
    a_zero_bit_prefix_matches_every_address_of_its_family();
    an_ipv6_range_matches_ipv6_addresses();
    an_ipv4_mapped_ipv6_address_matches_an_ipv4_range();
    comments_and_blank_lines_are_skipped();
    crlf_line_ends_are_accepted();
    an_empty_text_gives_an_empty_list();
    a_bad_address_gives_its_line_number();
    a_prefix_longer_than_the_address_gives_its_line_number();
    a_prefix_that_is_not_a_number_gives_its_line_number();
    two_ranges_on_one_line_give_its_line_number();
    a_line_after_the_last_free_range_gives_its_line_number();
    text_that_is_not_an_address_matches_nothing();
    printf("test_ip_list: ok\n");

    return 0;
}
