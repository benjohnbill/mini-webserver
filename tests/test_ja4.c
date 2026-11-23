#include <stdio.h>
#include <string.h>

#include "check.h"
#include "client_hello.h"
#include "ja4.h"

/* The expected values come from the FoxIO JA4 document,
   technical_details/JA4.md, sections "Example", "Cipher hash", "Extension
   hash", "TLS and DTLS Version", and "ALPN Extension Value". Each hash was
   checked with sha256sum. The s_client value comes from
   tests/data/openssl_client_hello.trace with the same rules. */

#define CHECK_JA4(hello, expected)                                             \
    do {                                                                       \
        char check_ja4[JA4_LEN + 1];                                           \
        ja4_format((hello), check_ja4);                                        \
        if (strcmp(check_ja4, (expected)) != 0) {                              \
            fprintf(stderr, "%s:%d: %s: JA4 is \"%s\", expected \"%s\"\n",     \
                    __FILE__, __LINE__, __func__, check_ja4, (expected));      \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* Checks JA4 characters [at, at + strlen(expected)). */
#define CHECK_JA4_PART(hello, at, expected)                                    \
    do {                                                                       \
        char check_ja4[JA4_LEN + 1];                                           \
        ja4_format((hello), check_ja4);                                        \
        if (strncmp(check_ja4 + (at), (expected), strlen(expected)) != 0) {    \
            fprintf(stderr,                                                    \
                    "%s:%d: %s: JA4 is \"%s\", expected \"%s\" at "            \
                    "%d\n",                                                    \
                    __FILE__, __LINE__, __func__, check_ja4, (expected),       \
                    (at));                                                     \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define VERSION_AT 1
#define ALPN_AT 8
#define CIPHER_HASH_AT 11
#define EXTENSION_HASH_AT 24

static const uint16_t spec_ciphers[] = {
    0x1301, 0x1302, 0x1303, 0xc02b, 0xc02f, 0xc02c, 0xc030, 0xcca9,
    0xcca8, 0xc013, 0xc014, 0x009c, 0x009d, 0x002f, 0x0035,
};
static const uint16_t spec_extensions[] = {
    0x001b, 0x0000, 0x0033, 0x0010, 0x4469, 0x0017, 0x002d, 0x000d,
    0x0005, 0x0023, 0x0012, 0x002b, 0xff01, 0x000b, 0x000a, 0x0015,
};
static const uint16_t spec_signatures[] = {
    0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601,
};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static void list_copy(uint16_t *list, size_t *count, const uint16_t *values,
                      size_t n)
{
    memcpy(list, values, n * sizeof(values[0]));
    *count = n;
}

static void alpn_set(struct client_hello *hello, const unsigned char *alpn,
                     size_t alpn_len)
{
    memcpy(hello->alpn, alpn, alpn_len);
    hello->alpn_len = alpn_len;
}

/* The ClientHello of the document's example. */
static void spec_hello_init(struct client_hello *hello)
{
    memset(hello, 0, sizeof(*hello));
    hello->legacy_version = 0x0303;
    list_copy(hello->ciphers, &hello->cipher_count, spec_ciphers,
              COUNT(spec_ciphers));
    list_copy(hello->extensions, &hello->extension_count, spec_extensions,
              COUNT(spec_extensions));
    list_copy(hello->signatures, &hello->signature_count, spec_signatures,
              COUNT(spec_signatures));
    hello->versions[0] = 0x0304;
    hello->version_count = 1;
    alpn_set(hello, (const unsigned char *)"h2", 2);
}

static void list_remove(uint16_t *list, size_t *count, uint16_t value)
{
    size_t kept = 0;
    for (size_t i = 0; i < *count; i++) {
        if (list[i] != value) {
            list[kept++] = list[i];
        }
    }
    *count = kept;
}

static void list_append(uint16_t *list, size_t *count, uint16_t value)
{
    list[(*count)++] = value;
}

static void the_document_example_gives_its_fingerprint(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    CHECK_JA4(&hello, "t13d1516h2_8daaf6152771_e5627efa2ab1");
}

static void the_openssl_s_client_hello_gives_its_fingerprint(void)
{
    static unsigned char bytes[4096];
    FILE *file = fopen("tests/data/openssl_client_hello.bin", "rb");
    CHECK(file != NULL);
    size_t len = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(bytes, len, &hello), CLIENT_HELLO_OK);
    CHECK_JA4(&hello, "t13d3013h2_1d37bd780c83_ecd0401ec68b");
}

static void grease_values_do_not_change_the_fingerprint(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    list_append(hello.ciphers, &hello.cipher_count, 0x0a0a);
    list_append(hello.extensions, &hello.extension_count, 0x1a1a);
    list_append(hello.extensions, &hello.extension_count, 0xfafa);
    list_append(hello.versions, &hello.version_count, 0x2a2a);
    CHECK_JA4(&hello, "t13d1516h2_8daaf6152771_e5627efa2ab1");
}

static void a_hello_without_sni_is_marked_i_and_keeps_its_hashes(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    list_remove(hello.extensions, &hello.extension_count, 0x0000);
    CHECK_JA4(&hello, "t13i1515h2_8daaf6152771_e5627efa2ab1");
}

static void the_highest_supported_version_wins_and_grease_is_not_one(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.version_count = 0;
    list_append(hello.versions, &hello.version_count, 0x7a7a);
    list_append(hello.versions, &hello.version_count, 0x0303);
    list_append(hello.versions, &hello.version_count, 0x0304);
    CHECK_JA4_PART(&hello, VERSION_AT, "13");
}

static void supported_versions_win_over_the_legacy_version(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.legacy_version = 0x0301;
    CHECK_JA4_PART(&hello, VERSION_AT, "13");
}

static void without_supported_versions_the_legacy_version_decides(void)
{
    static const struct {
        uint16_t version;
        const char *text;
    } cases[] = {
        {0x0304, "13"}, {0x0303, "12"}, {0x0302, "11"}, {0x0301, "10"},
        {0x0300, "s3"}, {0x0002, "s2"}, {0xfeff, "d1"}, {0xfefd, "d2"},
        {0xfefc, "d3"}, {0x0305, "00"},
    };
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.version_count = 0;
    for (size_t i = 0; i < COUNT(cases); i++) {
        hello.legacy_version = cases[i].version;
        CHECK_JA4_PART(&hello, VERSION_AT, cases[i].text);
    }
}

static void a_count_over_99_is_written_as_99(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.cipher_count = 0;
    for (uint16_t i = 0; i < 100; i++) {
        list_append(hello.ciphers, &hello.cipher_count, (uint16_t)(0x1000 + i));
    }
    CHECK_JA4_PART(&hello, 0, "t13d9916h2_");
}

static void alpn_uses_the_first_and_last_character(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    alpn_set(&hello, (const unsigned char *)"http/1.1", 8);
    CHECK_JA4_PART(&hello, ALPN_AT, "h1");
}

static void a_one_character_alpn_is_used_twice(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    alpn_set(&hello, (const unsigned char *)"x", 1);
    CHECK_JA4_PART(&hello, ALPN_AT, "xx");
}

static void no_alpn_is_written_as_00(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.alpn_len = 0;
    CHECK_JA4_PART(&hello, ALPN_AT, "00");
}

static void a_non_alphanumeric_alpn_end_uses_the_hex_ends(void)
{
    static const struct {
        unsigned char bytes[4];
        size_t len;
        const char *text;
    } cases[] = {
        {{0xab}, 1, "ab"},
        {{0x20}, 1, "20"},
        {{0xab, 0xcd}, 2, "ad"},
        {{0x20, 0x61}, 2, "21"},
        {{0x30, 0xab}, 2, "3b"},
        {{0x61, 0x20}, 2, "60"},
        {{0x30, 0x31, 0xab, 0xcd}, 4, "3d"},
        {{0x30, 0xab, 0xcd, 0x31}, 4, "01"},
    };
    struct client_hello hello;
    spec_hello_init(&hello);
    for (size_t i = 0; i < COUNT(cases); i++) {
        alpn_set(&hello, cases[i].bytes, cases[i].len);
        CHECK_JA4_PART(&hello, ALPN_AT, cases[i].text);
    }
}

static void no_ciphers_give_a_zero_cipher_hash(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.cipher_count = 0;
    CHECK_JA4(&hello, "t13d0016h2_000000000000_e5627efa2ab1");
}

static void no_signature_algorithms_drop_the_underscore_from_the_hash(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.signature_count = 0;
    CHECK_JA4_PART(&hello, EXTENSION_HASH_AT, "6d807ffa2a79");
}

static void only_sni_and_alpn_give_a_zero_extension_hash(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    hello.extension_count = 0;
    list_append(hello.extensions, &hello.extension_count, 0x0000);
    list_append(hello.extensions, &hello.extension_count, 0x0010);
    CHECK_JA4(&hello, "t13d1502h2_8daaf6152771_000000000000");
}

static void the_cipher_order_does_not_change_the_hash(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    uint16_t first = hello.ciphers[0];
    hello.ciphers[0] = hello.ciphers[14];
    hello.ciphers[14] = first;
    CHECK_JA4_PART(&hello, CIPHER_HASH_AT, "8daaf6152771");
}

static void the_signature_order_changes_the_hash(void)
{
    struct client_hello hello;
    spec_hello_init(&hello);
    uint16_t first = hello.signatures[0];
    hello.signatures[0] = hello.signatures[1];
    hello.signatures[1] = first;
    char ja4[JA4_LEN + 1];
    ja4_format(&hello, ja4);
    CHECK(strlen(ja4) == JA4_LEN);
    CHECK(strcmp(ja4 + EXTENSION_HASH_AT, "e5627efa2ab1") != 0);
}

int main(void)
{
    the_document_example_gives_its_fingerprint();
    the_openssl_s_client_hello_gives_its_fingerprint();
    grease_values_do_not_change_the_fingerprint();
    a_hello_without_sni_is_marked_i_and_keeps_its_hashes();
    the_highest_supported_version_wins_and_grease_is_not_one();
    supported_versions_win_over_the_legacy_version();
    without_supported_versions_the_legacy_version_decides();
    a_count_over_99_is_written_as_99();
    alpn_uses_the_first_and_last_character();
    a_one_character_alpn_is_used_twice();
    no_alpn_is_written_as_00();
    a_non_alphanumeric_alpn_end_uses_the_hex_ends();
    no_ciphers_give_a_zero_cipher_hash();
    no_signature_algorithms_drop_the_underscore_from_the_hash();
    only_sni_and_alpn_give_a_zero_extension_hash();
    the_cipher_order_does_not_change_the_hash();
    the_signature_order_changes_the_hash();
    printf("test_ja4: ok\n");

    return 0;
}
