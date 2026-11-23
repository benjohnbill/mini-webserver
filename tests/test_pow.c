#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "pow.h"

/* The expected values come from tools outside this code:
   printf '%s' '203.0.113.7|56666666' | openssl dgst -sha256 -hmac test-secret
   printf '%s' 'NONCE:COUNTER' | sha256sum
   The counters were found with a Python hashlib loop and checked with
   sha256sum. */

#define SECRET "test-secret"
#define CLIENT_IP "203.0.113.7"
/* 1700000000 / 30 = 56666666, rest 20. The window is 1699999980 to
   1700000010. */
#define NOW 1700000000.0

/* HMAC of "203.0.113.7|56666666", the window of NOW. */
#define NONCE_NOW                                                              \
    "ae97a46a64a864388e673d89ebd1663ba63305c5424566e26ebf6a5e427e2f92"
/* HMAC of "203.0.113.7|56666665", the window before. */
#define NONCE_BEFORE                                                           \
    "d493ade80825ef8a5c506cdafb4544d837ea322e1a67d9aaf9ac7808a82cc77d"
/* HMAC of "203.0.113.7|56666664", two windows before. */
#define NONCE_OLD                                                              \
    "6437916feff4390ac7115c6e9b9482c43308e49a9fc19e82e71cdbe81b58bf32"
/* HMAC of "203.0.113.8|56666666", another client. */
#define NONCE_OTHER_IP                                                         \
    "9db76533751bdc0aa7aa462194fc572ff20a73d06ca311069f8580c0dccaf0f1"

static bool solved(const char *solution, const char *client_ip, double now,
                   int bits)
{
    return pow_is_solved(solution, client_ip, now,
                         (const unsigned char *)SECRET, strlen(SECRET), bits);
}

static void the_challenge_is_the_hmac_in_hex_and_the_bits(void)
{
    char challenge[POW_CHALLENGE_MAX + 1];
    pow_format_challenge(CLIENT_IP, NOW, (const unsigned char *)SECRET,
                         strlen(SECRET), 8, challenge);
    CHECK(strcmp(challenge, NONCE_NOW " 8") == 0);
}

static void a_counter_with_enough_zero_bits_solves_the_challenge(void)
{
    /* sha256sum: 00ccfba8..., 8 zero bits. */
    CHECK(solved(NONCE_NOW ":69", CLIENT_IP, NOW, 8));
}

static void a_counter_with_too_few_zero_bits_does_not_solve_it(void)
{
    /* sha256sum: 01719116..., 7 zero bits. */
    CHECK(!solved(NONCE_NOW ":128", CLIENT_IP, NOW, 8));
    CHECK(solved(NONCE_NOW ":128", CLIENT_IP, NOW, 7));
}

static void zero_bits_are_counted_inside_a_byte(void)
{
    /* sha256sum: 00081e9a..., 12 zero bits. */
    CHECK(solved(NONCE_NOW ":6106", CLIENT_IP, NOW, 12));
    CHECK(!solved(NONCE_NOW ":6106", CLIENT_IP, NOW, 13));
}

static void the_nonce_of_the_window_before_is_accepted(void)
{
    /* sha256sum: 00dce893..., 8 zero bits. */
    CHECK(solved(NONCE_BEFORE ":63", CLIENT_IP, NOW, 8));
}

static void the_nonce_of_an_old_window_is_refused(void)
{
    /* sha256sum: 0058a3f1..., 9 zero bits: only the window is wrong. */
    CHECK(!solved(NONCE_OLD ":805", CLIENT_IP, NOW, 8));
}

static void a_nonce_stays_valid_for_one_more_window_only(void)
{
    CHECK(solved(NONCE_NOW ":69", CLIENT_IP, NOW + 10, 8));
    CHECK(solved(NONCE_NOW ":69", CLIENT_IP, NOW + 39, 8));
    CHECK(!solved(NONCE_NOW ":69", CLIENT_IP, NOW + 40, 8));
}

static void the_nonce_of_another_ip_is_refused(void)
{
    /* sha256sum: 00256e78..., 10 zero bits: only the IP is wrong. */
    CHECK(solved(NONCE_OTHER_IP ":122", "203.0.113.8", NOW, 8));
    CHECK(!solved(NONCE_OTHER_IP ":122", CLIENT_IP, NOW, 8));
}

static void a_nonce_from_another_secret_is_refused(void)
{
    const char *other = "other-secret";
    CHECK(!pow_is_solved(NONCE_NOW ":69", CLIENT_IP, NOW,
                         (const unsigned char *)other, strlen(other), 8));
}

static void a_malformed_solution_is_refused(void)
{
    CHECK(!solved(NULL, CLIENT_IP, NOW, 0));
    CHECK(!solved("", CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW, CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW ":", CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW " 69", CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW ":6x9", CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW ":-69", CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW ":123456789012345678901", CLIENT_IP, NOW, 0));
    CHECK(!solved("ae97a46a:69", CLIENT_IP, NOW, 0));
    CHECK(
        !solved("AE97A46A64A864388E673D89EBD1663BA63305C5424566E26EBF6A5E427E2F"
                "92:69",
                CLIENT_IP, NOW, 0));
}

static void bits_out_of_range_are_refused(void)
{
    CHECK(solved(NONCE_NOW ":69", CLIENT_IP, NOW, 0));
    CHECK(!solved(NONCE_NOW ":69", CLIENT_IP, NOW, -1));
    CHECK(!solved(NONCE_NOW ":69", CLIENT_IP, NOW, POW_BITS_MAX + 1));
}

int main(void)
{
    the_challenge_is_the_hmac_in_hex_and_the_bits();
    a_counter_with_enough_zero_bits_solves_the_challenge();
    a_counter_with_too_few_zero_bits_does_not_solve_it();
    zero_bits_are_counted_inside_a_byte();
    the_nonce_of_the_window_before_is_accepted();
    the_nonce_of_an_old_window_is_refused();
    a_nonce_stays_valid_for_one_more_window_only();
    the_nonce_of_another_ip_is_refused();
    a_nonce_from_another_secret_is_refused();
    a_malformed_solution_is_refused();
    bits_out_of_range_are_refused();
    printf("test_pow: ok\n");

    return 0;
}
