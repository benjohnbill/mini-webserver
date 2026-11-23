#include <stdio.h>
#include <string.h>

#include "check.h"
#include "client_hello.h"

/* tests/data/openssl_client_hello.bin is the ClientHello of
   "openssl s_client -servername example.com -alpn h2,http/1.1"
   (OpenSSL 3.5.5). openssl_client_hello.trace is the -trace output of the
   same connection. The expected values come from that trace. */

#define FILE_MAX 4096
#define RECORD_MAX 1024

static size_t fixture_read(const char *path, unsigned char *bytes)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    size_t len = fread(bytes, 1, FILE_MAX, file);
    fclose(file);

    return len;
}

static void put_u16(unsigned char *at, size_t value)
{
    at[0] = (unsigned char)(value >> 8);
    at[1] = (unsigned char)value;
}

/* Makes one record with a ClientHello: version 0x0303, a zero random, no
   session id, the ciphers, no compression, and the extension bytes. */
static size_t record_make(const uint16_t *ciphers, size_t cipher_count,
                          const unsigned char *extensions,
                          size_t extensions_len, unsigned char *record)
{
    unsigned char *body = record + 9;
    size_t at = 0;
    put_u16(body + at, 0x0303);
    at += 2;
    memset(body + at, 0, 32);
    at += 32;
    body[at++] = 0;
    put_u16(body + at, cipher_count * 2);
    at += 2;
    for (size_t i = 0; i < cipher_count; i++) {
        put_u16(body + at, ciphers[i]);
        at += 2;
    }
    body[at++] = 1;
    body[at++] = 0;
    put_u16(body + at, extensions_len);
    at += 2;
    memcpy(body + at, extensions, extensions_len);
    at += extensions_len;

    record[0] = 0x16;
    put_u16(record + 1, 0x0301);
    put_u16(record + 3, at + 4);
    record[5] = 1;
    record[6] = 0;
    put_u16(record + 7, at);

    return at + 9;
}

static enum client_hello_status openssl_hello_parse(struct client_hello *hello)
{
    static unsigned char bytes[FILE_MAX];
    size_t len = fixture_read("tests/data/openssl_client_hello.bin", bytes);

    return client_hello_parse(bytes, len, hello);
}

static void the_openssl_hello_parses(void)
{
    struct client_hello hello;
    CHECK_EQ_INT(openssl_hello_parse(&hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.legacy_version, 0x0303);
}

static void the_openssl_hello_keeps_the_ciphers_in_wire_order(void)
{
    struct client_hello hello;
    CHECK_EQ_INT(openssl_hello_parse(&hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.cipher_count, 30);
    CHECK_EQ_INT(hello.ciphers[0], 0x1302);
    CHECK_EQ_INT(hello.ciphers[3], 0xc02c);
    CHECK_EQ_INT(hello.ciphers[29], 0x002f);
}

static void the_openssl_hello_keeps_the_extensions_in_wire_order(void)
{
    static const uint16_t expected[] = {
        0xff01, 0x0000, 0x000b, 0x000a, 0x0023, 0x0010, 0x0016,
        0x0017, 0x000d, 0x002b, 0x002d, 0x0033, 0x001b,
    };
    struct client_hello hello;
    CHECK_EQ_INT(openssl_hello_parse(&hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.extension_count, 13);
    for (size_t i = 0; i < 13; i++) {
        CHECK_EQ_INT(hello.extensions[i], expected[i]);
    }
}

static void the_openssl_hello_gives_its_supported_versions(void)
{
    struct client_hello hello;
    CHECK_EQ_INT(openssl_hello_parse(&hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.version_count, 2);
    CHECK_EQ_INT(hello.versions[0], 0x0304);
    CHECK_EQ_INT(hello.versions[1], 0x0303);
}

static void the_openssl_hello_keeps_the_signature_algorithms_in_order(void)
{
    struct client_hello hello;
    CHECK_EQ_INT(openssl_hello_parse(&hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.signature_count, 25);
    CHECK_EQ_INT(hello.signatures[0], 0x0905);
    CHECK_EQ_INT(hello.signatures[3], 0x0403);
    CHECK_EQ_INT(hello.signatures[24], 0x0602);
}

static void the_openssl_hello_gives_the_first_alpn_protocol(void)
{
    struct client_hello hello;
    CHECK_EQ_INT(openssl_hello_parse(&hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.alpn_len, 2);
    CHECK(memcmp(hello.alpn, "h2", 2) == 0);
}

static void the_curl_hello_parses_and_asks_for_h2_first(void)
{
    static unsigned char bytes[FILE_MAX];
    size_t len = fixture_read("tests/data/curl_client_hello.bin", bytes);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(bytes, len, &hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.alpn_len, 2);
    CHECK(memcmp(hello.alpn, "h2", 2) == 0);
}

static void every_prefix_of_the_record_is_incomplete(void)
{
    static unsigned char bytes[FILE_MAX];
    size_t len = fixture_read("tests/data/openssl_client_hello.bin", bytes);
    struct client_hello hello;
    for (size_t prefix = 0; prefix < len; prefix++) {
        CHECK_EQ_INT(client_hello_parse(bytes, prefix, &hello),
                     CLIENT_HELLO_INCOMPLETE);
    }
}

static void a_plain_http_request_is_invalid(void)
{
    const char *text = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse((const unsigned char *)text,
                                    strlen(text), &hello),
                 CLIENT_HELLO_INVALID);
}

static void one_byte_of_plain_http_is_already_invalid(void)
{
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse((const unsigned char *)"G", 1, &hello),
                 CLIENT_HELLO_INVALID);
}

static void a_handshake_that_is_not_a_client_hello_is_invalid(void)
{
    static const uint16_t ciphers[] = {0x1301};
    unsigned char record[RECORD_MAX];
    size_t len = record_make(ciphers, 1, NULL, 0, record);
    record[5] = 2; /* ServerHello */
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello),
                 CLIENT_HELLO_INVALID);
}

static void grease_values_stay_in_the_lists(void)
{
    static const uint16_t ciphers[] = {0x0a0a, 0x1301};
    static const unsigned char extensions[] = {0x1a, 0x1a, 0x00, 0x00};
    unsigned char record[RECORD_MAX];
    size_t len = record_make(ciphers, 2, extensions, sizeof(extensions),
                             record);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.cipher_count, 2);
    CHECK_EQ_INT(hello.ciphers[0], 0x0a0a);
    CHECK_EQ_INT(hello.extension_count, 1);
    CHECK_EQ_INT(hello.extensions[0], 0x1a1a);
}

static void a_hello_without_extensions_has_empty_lists(void)
{
    static const uint16_t ciphers[] = {0x1301};
    unsigned char record[RECORD_MAX];
    size_t len = record_make(ciphers, 1, NULL, 0, record);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.extension_count, 0);
    CHECK_EQ_INT(hello.version_count, 0);
    CHECK_EQ_INT(hello.signature_count, 0);
    CHECK_EQ_INT(hello.alpn_len, 0);
}

static void an_extension_longer_than_the_list_is_invalid(void)
{
    static const uint16_t ciphers[] = {0x1301};
    /* type 0x0017, length 8, but only 2 bytes of data follow. */
    static const unsigned char extensions[] = {0x00, 0x17, 0x00, 0x08,
                                               0x00, 0x00};
    unsigned char record[RECORD_MAX];
    size_t len = record_make(ciphers, 1, extensions, sizeof(extensions),
                             record);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello),
                 CLIENT_HELLO_INVALID);
}

static void an_alpn_name_longer_than_its_extension_is_invalid(void)
{
    static const uint16_t ciphers[] = {0x1301};
    /* ALPN, length 5, list length 3, name length 9. */
    static const unsigned char extensions[] = {0x00, 0x10, 0x00, 0x05, 0x00,
                                               0x03, 0x09, 0x68, 0x32};
    unsigned char record[RECORD_MAX];
    size_t len = record_make(ciphers, 1, extensions, sizeof(extensions),
                             record);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello),
                 CLIENT_HELLO_INVALID);
}

static void more_ciphers_than_the_max_is_invalid(void)
{
    static uint16_t ciphers[CLIENT_HELLO_CIPHER_MAX + 1];
    for (size_t i = 0; i <= CLIENT_HELLO_CIPHER_MAX; i++) {
        ciphers[i] = (uint16_t)(0x1000 + i);
    }
    unsigned char record[RECORD_MAX];
    size_t len = record_make(ciphers, CLIENT_HELLO_CIPHER_MAX + 1, NULL, 0,
                             record);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello),
                 CLIENT_HELLO_INVALID);
}

static void exactly_the_max_of_ciphers_parses(void)
{
    static uint16_t ciphers[CLIENT_HELLO_CIPHER_MAX];
    for (size_t i = 0; i < CLIENT_HELLO_CIPHER_MAX; i++) {
        ciphers[i] = (uint16_t)(0x1000 + i);
    }
    unsigned char record[RECORD_MAX];
    size_t len =
        record_make(ciphers, CLIENT_HELLO_CIPHER_MAX, NULL, 0, record);
    struct client_hello hello;
    CHECK_EQ_INT(client_hello_parse(record, len, &hello), CLIENT_HELLO_OK);
    CHECK_EQ_INT(hello.cipher_count, CLIENT_HELLO_CIPHER_MAX);
    CHECK_EQ_INT(hello.ciphers[CLIENT_HELLO_CIPHER_MAX - 1], 0x107f);
}

int main(void)
{
    the_openssl_hello_parses();
    the_openssl_hello_keeps_the_ciphers_in_wire_order();
    the_openssl_hello_keeps_the_extensions_in_wire_order();
    the_openssl_hello_gives_its_supported_versions();
    the_openssl_hello_keeps_the_signature_algorithms_in_order();
    the_openssl_hello_gives_the_first_alpn_protocol();
    the_curl_hello_parses_and_asks_for_h2_first();
    every_prefix_of_the_record_is_incomplete();
    a_plain_http_request_is_invalid();
    one_byte_of_plain_http_is_already_invalid();
    a_handshake_that_is_not_a_client_hello_is_invalid();
    grease_values_stay_in_the_lists();
    a_hello_without_extensions_has_empty_lists();
    an_extension_longer_than_the_list_is_invalid();
    an_alpn_name_longer_than_its_extension_is_invalid();
    more_ciphers_than_the_max_is_invalid();
    exactly_the_max_of_ciphers_parses();
    printf("test_client_hello: ok\n");

    return 0;
}
