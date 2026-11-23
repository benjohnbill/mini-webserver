#ifndef BOT_AUTH_H
#define BOT_AUTH_H

#include <stdbool.h>
#include <stddef.h>

/* Decision module of the Reverse Proxy: Web Bot Auth. It verifies an
   RFC 9421 HTTP Message Signature with Ed25519 (OpenSSL EVP, no I/O).
   Time is an argument. The caller gives the header lookup, so this module
   does not know struct request. */

#define BOT_AUTH_KEY_MAX 64
/* A JWK SHA-256 thumbprint (RFC 7638) has 43 characters. */
#define BOT_AUTH_KEYID_MAX 64
#define BOT_AUTH_PUBLIC_KEY_LEN 32
/* A created time up to this far in the future passes. The default
   clockskew of MIT Kerberos (krb5.conf) is 300 s. */
#define BOT_AUTH_SKEW_SECONDS 300
/* A signature with no expires expires this long after created. The Web
   Bot Auth draft (draft-meunier-web-bot-auth-architecture-05, section 4.2)
   recommends an expiry of no more than 24 hours. */
#define BOT_AUTH_MAX_AGE_SECONDS 86400

struct bot_key {
    char keyid[BOT_AUTH_KEYID_MAX + 1];
    unsigned char public_key[BOT_AUTH_PUBLIC_KEY_LEN];
};

struct bot_key_set {
    struct bot_key keys[BOT_AUTH_KEY_MAX];
    size_t count;
};

/* The request, as the signature sees it. */
struct bot_auth_message {
    const char *method;    /* "GET" */
    const char *authority; /* the Host value, "example.com" */
    const char *target;    /* the request target, "/foo?a=b" */
    /* Returns the value of the header name (lowercase), with no spaces
       around it, or NULL. main.c can wrap http_find_header. */
    const char *(*find_header)(const void *context, const char *name);
    const void *context;
};

enum bot_auth_result {
    BOT_AUTH_VERIFIED,
    BOT_AUTH_ABSENT,        /* no Signature-Input or no Signature */
    BOT_AUTH_MALFORMED,     /* a header does not parse, or a covered
                               component is not in the request */
    BOT_AUTH_UNSUPPORTED,   /* alg is not ed25519, a component is not
                               supported, or @authority is not covered */
    BOT_AUTH_UNKNOWN_KEY,   /* keyid is not in the key set */
    BOT_AUTH_EXPIRED,       /* now is at or after the expiry */
    BOT_AUTH_NOT_YET_VALID, /* created is after now + the skew */
    BOT_AUTH_BAD_SIGNATURE,
};

/* Reads text, the --bot-key file, into keys. One key on each line:
   "KEYID BASE64", separated by spaces or tabs. BASE64 is the raw 32-byte
   Ed25519 public key in standard base64 (44 characters). "#" starts a
   comment to the end of the line. Blank lines are skipped. Returns 0, or
   the number (from 1) of the first bad line. A line after
   BOT_AUTH_KEY_MAX keys is bad. */
size_t bot_auth_parse_keys(const char *text, struct bot_key_set *keys);

/* Verifies the first signature that Signature-Input names. Supported
   components: @method, @authority, @path, @query, @request-target, and
   header fields, with the parameter key="..." for a member of a
   Dictionary header (Signature-Agent). The signature must cover
   @authority and have created. When the result is BOT_AUTH_VERIFIED and
   keyid is not NULL, writes the keyid into keyid, which has
   BOT_AUTH_KEYID_MAX + 1 bytes. now is seconds since the Epoch. */
enum bot_auth_result bot_auth_decide(const struct bot_auth_message *message,
                                     const struct bot_key_set *keys, double now,
                                     char *keyid);

#endif
