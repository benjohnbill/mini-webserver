#include "pow.h"

#include <stdio.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

/* The HMAC message: "CLIENT_IP|WINDOW". An IPv6 address has at most 45
   characters, and a window at most 20 digits. */
#define MESSAGE_MAX 72
#define COUNTER_DIGITS_MAX 20
#define BITS_IN_BYTE 8

static long long window_get(double now);
static void nonce_format(const char *client_ip, long long window,
                         const unsigned char *secret, size_t secret_len,
                         char *nonce);
static bool counter_is_valid(const char *counter);
static bool nonce_is_issued(const char *nonce, const char *client_ip,
                            double now, const unsigned char *secret,
                            size_t secret_len);
static bool hash_has_zero_bits(const char *solution, int bits);

void pow_format_challenge(const char *client_ip, double now,
                          const unsigned char *secret, size_t secret_len,
                          int bits, char *challenge)
{
    char nonce[POW_NONCE_LEN + 1];
    nonce_format(client_ip, window_get(now), secret, secret_len, nonce);
    snprintf(challenge, POW_CHALLENGE_MAX + 1, "%s %d", nonce, bits);
}

bool pow_is_solved(const char *solution, const char *client_ip, double now,
                   const unsigned char *secret, size_t secret_len, int bits)
{
    if (solution == NULL || bits < 0 || bits > POW_BITS_MAX) {
        return false;
    }
    if (strlen(solution) <= POW_NONCE_LEN || solution[POW_NONCE_LEN] != ':' ||
        !counter_is_valid(solution + POW_NONCE_LEN + 1)) {
        return false;
    }
    char nonce[POW_NONCE_LEN + 1];
    memcpy(nonce, solution, POW_NONCE_LEN);
    nonce[POW_NONCE_LEN] = '\0';
    if (!nonce_is_issued(nonce, client_ip, now, secret, secret_len)) {
        return false;
    }

    return hash_has_zero_bits(solution, bits);
}

/* now is not negative, so the cast is the floor. */
static long long window_get(double now)
{
    return (long long)(now / POW_WINDOW_SECONDS);
}

static void nonce_format(const char *client_ip, long long window,
                         const unsigned char *secret, size_t secret_len,
                         char *nonce)
{
    char message[MESSAGE_MAX + 1];
    int message_len =
        snprintf(message, sizeof(message), "%s|%lld", client_ip, window);
    unsigned char mac[SHA256_DIGEST_LENGTH];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), secret, (int)secret_len, (unsigned char *)message,
         (size_t)message_len, mac, &mac_len);
    for (unsigned int i = 0; i < mac_len; i++) {
        snprintf(nonce + 2 * i, 3, "%02x", mac[i]);
    }
}

static bool counter_is_valid(const char *counter)
{
    size_t len = strlen(counter);
    if (len == 0 || len > COUNTER_DIGITS_MAX) {
        return false;
    }

    return strspn(counter, "0123456789") == len;
}

/* The nonce of the window before also passes: a client that got the
   challenge at the end of a window still has time to solve it. */
static bool nonce_is_issued(const char *nonce, const char *client_ip,
                            double now, const unsigned char *secret,
                            size_t secret_len)
{
    long long window = window_get(now);
    for (long long back = 0; back <= 1; back++) {
        char expected[POW_NONCE_LEN + 1];
        nonce_format(client_ip, window - back, secret, secret_len, expected);
        if (CRYPTO_memcmp(nonce, expected, POW_NONCE_LEN) == 0) {
            return true;
        }
    }

    return false;
}

static bool hash_has_zero_bits(const char *solution, int bits)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)solution, strlen(solution), hash);
    int full_bytes = bits / BITS_IN_BYTE;
    for (int i = 0; i < full_bytes; i++) {
        if (hash[i] != 0) {
            return false;
        }
    }
    int rest = bits % BITS_IN_BYTE;
    if (rest == 0) {
        return true;
    }
    unsigned int mask = (0xffU << (BITS_IN_BYTE - rest)) & 0xffU;

    return (hash[full_bytes] & mask) == 0;
}
