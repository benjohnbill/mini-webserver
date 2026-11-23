#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bot_auth.h"
#include "check.h"

/* Test vectors:
   - RFC 9421 Appendix B.2.6, key B.1.4 test-key-ed25519.
   - draft-meunier-web-bot-auth-architecture-05 Appendix A.2.1, the same
     key. Its keyid is the JWK thumbprint of the key.
   - Our own key, signed with
     openssl pkeyutl -sign -inkey bot_priv.pem -rawin -in base.txt
     over "@authority" and "signature-agent". openssl pkeyutl -verify
     checked each signature before it went here. */

/* openssl pkey -pubin -outform DER | tail -c 32 | base64 */
#define RFC_KEY "JrQLj5P/89iXES9+vFgrIy29clF9CC/oPPsw3c5D0bs="
#define OWN_KEY "D+mBW3KMy54ixzz40V91orHThxDwmUrtQHeTZReUNq0="
#define DRAFT_KEYID "poqkLGiymh_W0uP6PZFw-dvez3QJT5SolqXBCW38r0U"
#define OWN_KEYID "C40IKiW5iEzzt2B5DdKbd4pP26xcG8mbp_gDsQ93cnQ"

#define KEY_FILE                                                               \
    "# keyid base64\n"                                                         \
    "test-key-ed25519 " RFC_KEY "\n"                                           \
    "\n" DRAFT_KEYID "\t" RFC_KEY "  # the same key\r\n" OWN_KEYID " " OWN_KEY

#define RFC_CREATED 1618884473.0
#define RFC_INPUT                                                              \
    "sig-b26=(\"date\" \"@method\" \"@path\" \"@authority\" "                  \
    "\"content-type\" \"content-length\");created=1618884473;"                 \
    "keyid=\"test-key-ed25519\""
#define RFC_SIGNATURE                                                          \
    "sig-b26=:wqcAqbmYJ2ji2glfAMaRy4gruYYnx2nEFN2HN6jrnDnQCK1u02Gb04v9EDgwUPi" \
    "u4A0w6vuQv5lIp5WPpBKRCw==:"

#define DRAFT_CREATED 1735689600.0
#define DRAFT_INPUT                                                            \
    "sig1=(\"@authority\");created=1735689600;keyid=\"" DRAFT_KEYID            \
    "\";alg=\"ed25519\";expires=4889289600;nonce=\"g0iqFa9e1ffijlyOScDkXpfSmT" \
    "bYpRNSGPJrQ1It20ahwgzB3jOUcdgLgFxUg7RMtW4V8IILaKKtA+YuSyIgJQ==\";"        \
    "tag=\"web-bot-auth\""
#define DRAFT_SIGNATURE                                                        \
    "sig1=:FFASViSdcgsyaqqYiCnkHreeZzbNKcTzDvZC5uVlP/dn9IbWj8j0o4wKFTH3rBnUiS" \
    "UBduwm1Gp5VlIPCp01Ag==:"

#define OWN_CREATED 1700000000.0
#define OWN_EXPIRES 1700000300.0
#define OWN_AUTHORITY "rproxy.test:8443"
#define OWN_AGENT                                                              \
    "\"https://crawler.example/.well-known/"                                   \
    "http-message-signatures-directory\""
#define OWN_PARAMS                                                             \
    ";created=1700000000;expires=1700000300;keyid=\"" OWN_KEYID                \
    "\";alg=\"ed25519\";tag=\"web-bot-auth\""
#define OWN_INPUT                                                              \
    "sig1=(\"@authority\" \"signature-agent\";key=\"agent1\")" OWN_PARAMS
#define OWN_SIGNATURE_VALUE                                                    \
    ":HzhxB1UgogQCGCJ8yTBswQKmZaOpd3l2XdC6EL4WA0UjmyFwOEuNGCep34VGsIQ3ooN12g"  \
    "PSGtN0YD0XR3t/CQ==:"
#define OWN_SIGNATURE "sig1=" OWN_SIGNATURE_VALUE
#define LEGACY_INPUT "sig1=(\"@authority\" \"signature-agent\")" OWN_PARAMS
#define LEGACY_SIGNATURE                                                       \
    "sig1=:U3Mkme3xZKgx+RA5btp5JmNbEhKC7vRIbMSlvXqRgBKkyhROHKRIfoUI/ZcbwqEA35" \
    "rOEG8qryHI29DkiV++CQ==:"

#define HEADER_COUNT_MAX 8

struct header {
    const char *name;
    const char *value;
};

struct test_request {
    struct header headers[HEADER_COUNT_MAX];
    struct bot_auth_message message;
};

static struct bot_key_set keys;

static const char *headers_find(const void *context, const char *name)
{
    const struct header *headers = context;
    for (size_t i = 0; i < HEADER_COUNT_MAX && headers[i].name != NULL; i++) {
        if (strcasecmp(headers[i].name, name) == 0) {
            return headers[i].value;
        }
    }

    return NULL;
}

static void request_set_message(struct test_request *request,
                                const char *method, const char *authority,
                                const char *target)
{
    request->message.method = method;
    request->message.authority = authority;
    request->message.target = target;
    request->message.find_header = headers_find;
    request->message.context = request->headers;
}

/* POST /foo?param=Value&Pet=dog of RFC 9421 Appendix B.2. */
static void rfc_request_init(struct test_request *request,
                             const char *content_length)
{
    *request =
        (struct test_request){.headers = {
                                  {"Date", "Tue, 20 Apr 2021 02:07:55 GMT"},
                                  {"Content-Type", "application/json"},
                                  {"Content-Length", content_length},
                                  {"Signature-Input", RFC_INPUT},
                                  {"Signature", RFC_SIGNATURE},
                              }};
    request_set_message(request, "POST", "example.com",
                        "/foo?param=Value&Pet=dog");
}

static void own_request_init(struct test_request *request, const char *agent,
                             const char *input, const char *signature)
{
    *request = (struct test_request){.headers = {
                                         {"Signature-Agent", agent},
                                         {"Signature-Input", input},
                                         {"Signature", signature},
                                     }};
    request_set_message(request, "GET", OWN_AUTHORITY, "/data/1.html");
}

static enum bot_auth_result decide(const struct test_request *request,
                                   double now)
{
    return bot_auth_decide(&request->message, &keys, now, NULL);
}

static void the_key_file_parses(void)
{
    CHECK_EQ_INT(bot_auth_parse_keys(KEY_FILE, &keys), 0);
    CHECK_EQ_INT(keys.count, 3);
    CHECK(strcmp(keys.keys[0].keyid, "test-key-ed25519") == 0);
    CHECK(strcmp(keys.keys[1].keyid, DRAFT_KEYID) == 0);
    CHECK(strcmp(keys.keys[2].keyid, OWN_KEYID) == 0);
    /* base64 -d | xxd: 26 b4 0b ... d1 bb */
    CHECK_EQ_INT(keys.keys[0].public_key[0], 0x26);
    CHECK_EQ_INT(keys.keys[0].public_key[31], 0xbb);
}

static void a_bad_key_line_gives_its_number(void)
{
    struct bot_key_set bad_keys;
    CHECK_EQ_INT(bot_auth_parse_keys("a " RFC_KEY "\nb\n", &bad_keys), 2);
    CHECK_EQ_INT(bot_auth_parse_keys("a " RFC_KEY " extra\n", &bad_keys), 1);
    /* 33 bytes, not 32. */
    CHECK_EQ_INT(
        bot_auth_parse_keys("\na JrQLj5P/89iXES9+vFgrIy29clF9CC/oPPsw3c5D0bsA",
                            &bad_keys),
        2);
    CHECK_EQ_INT(bot_auth_parse_keys("a " RFC_KEY "!\n", &bad_keys), 1);
}

static void the_rfc_9421_vector_verifies(void)
{
    struct test_request request;
    rfc_request_init(&request, "18");
    char keyid[BOT_AUTH_KEYID_MAX + 1] = "";
    CHECK_EQ_INT(
        bot_auth_decide(&request.message, &keys, RFC_CREATED + 10, keyid),
        BOT_AUTH_VERIFIED);
    CHECK(strcmp(keyid, "test-key-ed25519") == 0);
}

static void one_changed_byte_in_the_base_fails(void)
{
    struct test_request request;
    rfc_request_init(&request, "19");
    CHECK_EQ_INT(decide(&request, RFC_CREATED + 10), BOT_AUTH_BAD_SIGNATURE);
    rfc_request_init(&request, "18");
    request.message.method = "PUT";
    CHECK_EQ_INT(decide(&request, RFC_CREATED + 10), BOT_AUTH_BAD_SIGNATURE);
    rfc_request_init(&request, "18");
    request.message.target = "/foo?param=Value&Pet=cat";
    CHECK_EQ_INT(decide(&request, RFC_CREATED + 10), BOT_AUTH_VERIFIED);
    request.message.target = "/fob?param=Value&Pet=dog";
    CHECK_EQ_INT(decide(&request, RFC_CREATED + 10), BOT_AUTH_BAD_SIGNATURE);
}

static void the_authority_is_lowercase_with_no_default_port(void)
{
    struct test_request request;
    rfc_request_init(&request, "18");
    request.message.authority = "Example.COM:443";
    CHECK_EQ_INT(decide(&request, RFC_CREATED + 10), BOT_AUTH_VERIFIED);
    request.message.authority = "example.com:8443";
    CHECK_EQ_INT(decide(&request, RFC_CREATED + 10), BOT_AUTH_BAD_SIGNATURE);
}

static void an_unknown_keyid_fails(void)
{
    struct bot_key_set other_keys;
    CHECK_EQ_INT(bot_auth_parse_keys("other-key " RFC_KEY, &other_keys), 0);
    struct test_request request;
    rfc_request_init(&request, "18");
    CHECK_EQ_INT(
        bot_auth_decide(&request.message, &other_keys, RFC_CREATED + 10, NULL),
        BOT_AUTH_UNKNOWN_KEY);
}

static void a_known_keyid_with_another_key_fails(void)
{
    struct bot_key_set other_keys;
    CHECK_EQ_INT(bot_auth_parse_keys("test-key-ed25519 " OWN_KEY, &other_keys),
                 0);
    struct test_request request;
    rfc_request_init(&request, "18");
    CHECK_EQ_INT(
        bot_auth_decide(&request.message, &other_keys, RFC_CREATED + 10, NULL),
        BOT_AUTH_BAD_SIGNATURE);
}

static void the_web_bot_auth_draft_vector_verifies(void)
{
    struct test_request request = {.headers = {
                                       {"Signature-Input", DRAFT_INPUT},
                                       {"Signature", DRAFT_SIGNATURE},
                                   }};
    request_set_message(&request, "GET", "example.com", "/");
    CHECK_EQ_INT(decide(&request, DRAFT_CREATED + 10), BOT_AUTH_VERIFIED);
}

static void a_signature_over_authority_and_signature_agent_verifies(void)
{
    struct test_request request;
    own_request_init(&request, "agent1=" OWN_AGENT, OWN_INPUT, OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED + 10), BOT_AUTH_VERIFIED);
    own_request_init(&request, "agent0=\"x\", agent1=" OWN_AGENT, OWN_INPUT,
                     OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED + 10), BOT_AUTH_VERIFIED);
}

static void a_changed_signature_agent_fails(void)
{
    struct test_request request;
    own_request_init(&request, "agent1=\"https://evil.example/\"", OWN_INPUT,
                     OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED + 10), BOT_AUTH_BAD_SIGNATURE);
}

static void a_signature_agent_without_the_signed_member_fails(void)
{
    struct test_request request;
    own_request_init(&request, "agent2=" OWN_AGENT, OWN_INPUT, OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED + 10), BOT_AUTH_MALFORMED);
}

static void a_legacy_string_signature_agent_verifies(void)
{
    struct test_request request;
    own_request_init(&request, OWN_AGENT, LEGACY_INPUT, LEGACY_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED + 10), BOT_AUTH_VERIFIED);
}

static void an_expired_signature_fails(void)
{
    struct test_request request;
    own_request_init(&request, "agent1=" OWN_AGENT, OWN_INPUT, OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_EXPIRES - 1), BOT_AUTH_VERIFIED);
    CHECK_EQ_INT(decide(&request, OWN_EXPIRES), BOT_AUTH_EXPIRED);
}

static void a_signature_with_no_expires_expires_after_a_day(void)
{
    struct test_request request;
    rfc_request_init(&request, "18");
    CHECK_EQ_INT(decide(&request, RFC_CREATED + BOT_AUTH_MAX_AGE_SECONDS - 1),
                 BOT_AUTH_VERIFIED);
    CHECK_EQ_INT(decide(&request, RFC_CREATED + BOT_AUTH_MAX_AGE_SECONDS),
                 BOT_AUTH_EXPIRED);
}

static void a_created_time_in_the_future_beyond_the_skew_fails(void)
{
    struct test_request request;
    own_request_init(&request, "agent1=" OWN_AGENT, OWN_INPUT, OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED - BOT_AUTH_SKEW_SECONDS),
                 BOT_AUTH_VERIFIED);
    CHECK_EQ_INT(decide(&request, OWN_CREATED - BOT_AUTH_SKEW_SECONDS - 1),
                 BOT_AUTH_NOT_YET_VALID);
}

static void a_request_with_no_signature_is_absent(void)
{
    struct test_request request = {.headers = {{"Signature-Input", RFC_INPUT}}};
    request_set_message(&request, "GET", "example.com", "/");
    CHECK_EQ_INT(decide(&request, RFC_CREATED), BOT_AUTH_ABSENT);
    request = (struct test_request){.headers = {{"Signature", RFC_SIGNATURE}}};
    request_set_message(&request, "GET", "example.com", "/");
    CHECK_EQ_INT(decide(&request, RFC_CREATED), BOT_AUTH_ABSENT);
}

static void a_malformed_signature_header_fails(void)
{
    struct test_request request;
    own_request_init(&request, "agent1=" OWN_AGENT, "sig1=\"@authority\"",
                     OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_MALFORMED);
    own_request_init(&request, "agent1=" OWN_AGENT,
                     "sig1=(\"@authority\";created=1", OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_MALFORMED);
    own_request_init(&request, "agent1=" OWN_AGENT,
                     "sig1=(\"@authority\");keyid=\"" OWN_KEYID "\"",
                     OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_MALFORMED);
    own_request_init(&request, "agent1=" OWN_AGENT, OWN_INPUT, "sig1=:AAAA:");
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_MALFORMED);
    own_request_init(&request, "agent1=" OWN_AGENT, OWN_INPUT,
                     "sig2=" OWN_SIGNATURE_VALUE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_MALFORMED);
}

static void another_alg_or_no_authority_is_unsupported(void)
{
    struct test_request request;
    own_request_init(
        &request, "agent1=" OWN_AGENT,
        "sig1=(\"@authority\");created=1700000000;keyid=\"" OWN_KEYID
        "\";alg=\"rsa-pss-sha512\"",
        OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_UNSUPPORTED);
    own_request_init(&request, "agent1=" OWN_AGENT,
                     "sig1=(\"@method\");created=1700000000;keyid=\"" OWN_KEYID
                     "\"",
                     OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_UNSUPPORTED);
    own_request_init(&request, "agent1=" OWN_AGENT,
                     "sig1=(\"@authority\" \"@target-uri\");created=1700000000;"
                     "keyid=\"" OWN_KEYID "\"",
                     OWN_SIGNATURE);
    CHECK_EQ_INT(decide(&request, OWN_CREATED), BOT_AUTH_UNSUPPORTED);
}

int main(void)
{
    the_key_file_parses();
    a_bad_key_line_gives_its_number();
    the_rfc_9421_vector_verifies();
    one_changed_byte_in_the_base_fails();
    the_authority_is_lowercase_with_no_default_port();
    an_unknown_keyid_fails();
    a_known_keyid_with_another_key_fails();
    the_web_bot_auth_draft_vector_verifies();
    a_signature_over_authority_and_signature_agent_verifies();
    a_changed_signature_agent_fails();
    a_signature_agent_without_the_signed_member_fails();
    a_legacy_string_signature_agent_verifies();
    an_expired_signature_fails();
    a_signature_with_no_expires_expires_after_a_day();
    a_created_time_in_the_future_beyond_the_skew_fails();
    a_request_with_no_signature_is_absent();
    a_malformed_signature_header_fails();
    another_alg_or_no_authority_is_unsupported();
    printf("test_bot_auth: ok\n");

    return 0;
}
