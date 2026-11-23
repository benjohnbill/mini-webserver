#include "bot_auth.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

/* Sources: RFC 9421 (HTTP Message Signatures), RFC 8941 (Structured
   Fields), draft-meunier-web-bot-auth-architecture-05 (Web Bot Auth). The
   Structured Field parse reads only what a signature needs. */

#define LINE_MAX_LEN 255
#define BLANKS " \t\r"
#define KEY_BASE64_LEN 44
#define SIGNATURE_LEN 64
/* 64 bytes are 88 base64 characters. */
#define SIGNATURE_BASE64_LEN 88
#define BASE64_DECODED_MAX 96
#define BASE_MAX 4096
#define PARAMS_MAX 1024
#define COMPONENT_MAX 16
#define LABEL_MAX 64
#define COMPONENT_NAME_MAX 64
#define PARAM_TEXT_MAX 256
#define INTEGER_DIGITS_MAX 15
#define AUTHORITY_MAX 255
/* RFC 8941 section 3.2: the characters of a Dictionary key. */
#define KEY_CHARS "abcdefghijklmnopqrstuvwxyz0123456789_-.*"
#define ALG_ED25519 "ed25519"
#define AUTHORITY_COMPONENT "\"@authority\""
/* RFC 9421 section 2.1.2: a member of a Dictionary header. */
#define KEY_PARAM ";key="
/* rproxy serves HTTPS. RFC 9421 section 2.2.3 omits the default port. */
#define HTTPS_DEFAULT_PORT ":443"
#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7e

/* One member of Signature-Input, parsed. */
struct signature_input {
    char label[LABEL_MAX + 1];
    /* The member value, "(...);created=...". It is the value of
       @signature-params: a sender that serializes by RFC 8941 sends the
       same bytes that it signed. */
    char params[PARAMS_MAX + 1];
    /* The raw text of each covered component, in params. */
    const char *components[COMPONENT_MAX];
    size_t component_lens[COMPONENT_MAX];
    size_t component_count;
    long long created;
    bool has_created;
    long long expires;
    bool has_expires;
    char keyid[BOT_AUTH_KEYID_MAX + 1];
    char alg[PARAM_TEXT_MAX + 1];
};

/* The signature base, as it grows. */
struct base_text {
    char bytes[BASE_MAX];
    size_t len;
    bool is_full;
};

static size_t line_measure(const char *start);
static char *line_trim(char *line);
static bool key_parse(char *text, struct bot_key *key);
static bool base64_decode(const char *text, size_t len, unsigned char *out,
                          size_t out_len);
static bool signature_input_parse(const char *text,
                                  struct signature_input *input);
static bool dictionary_next(const char **cursor, char *key, const char **value,
                            size_t *value_len);
static size_t member_measure(const char *text);
static bool inner_list_parse(const char **cursor,
                             struct signature_input *input);
static bool string_parse(const char **cursor, char *out, size_t out_max);
static bool params_skip(const char **cursor);
static bool param_parse(const char **cursor, char *name, char *text,
                        long long *number, bool *is_number);
static bool integer_parse(const char **cursor, long long *number);
static bool input_param_set(struct signature_input *input, const char *name,
                            const char *text, long long number, bool is_number);
static enum bot_auth_result input_check(const struct signature_input *input,
                                        double now);
static bool input_covers_authority(const struct signature_input *input);
static const struct bot_key *key_find(const struct bot_key_set *keys,
                                      const char *keyid);
static bool signature_parse(const char *text, const char *label,
                            unsigned char *signature);
static enum bot_auth_result base_format(const struct bot_auth_message *message,
                                        const struct signature_input *input,
                                        struct base_text *base);
static enum bot_auth_result
component_append(const struct bot_auth_message *message, const char *raw,
                 size_t raw_len, struct base_text *base);
static enum bot_auth_result
derived_append(const struct bot_auth_message *message, const char *name,
               struct base_text *base);
static void authority_normalize(const char *authority, char *normalized);
static bool member_find(const char *text, const char *key, const char **value,
                        size_t *value_len);
static void base_append(struct base_text *base, const char *text, size_t len);
static void base_append_text(struct base_text *base, const char *text);
static bool signature_is_valid(const unsigned char *public_key,
                               const unsigned char *signature,
                               const struct base_text *base);

size_t bot_auth_parse_keys(const char *text, struct bot_key_set *keys)
{
    char line[LINE_MAX_LEN + 1];
    size_t line_number = 0;
    keys->count = 0;
    while (*text != '\0') {
        line_number++;
        size_t len = line_measure(text);
        if (len > LINE_MAX_LEN) {
            return line_number;
        }
        memcpy(line, text, len);
        line[len] = '\0';
        text += text[len] == '\n' ? len + 1 : len;
        char *key_text = line_trim(line);
        if (*key_text == '\0') {
            continue;
        }
        if (keys->count == BOT_AUTH_KEY_MAX ||
            !key_parse(key_text, &keys->keys[keys->count])) {
            return line_number;
        }
        keys->count++;
    }

    return 0;
}

enum bot_auth_result bot_auth_decide(const struct bot_auth_message *message,
                                     const struct bot_key_set *keys, double now,
                                     char *keyid)
{
    const char *input_text =
        message->find_header(message->context, "signature-input");
    const char *signature_text =
        message->find_header(message->context, "signature");
    if (input_text == NULL || signature_text == NULL) {
        return BOT_AUTH_ABSENT;
    }
    struct signature_input input;
    if (!signature_input_parse(input_text, &input)) {
        return BOT_AUTH_MALFORMED;
    }
    enum bot_auth_result result = input_check(&input, now);
    if (result != BOT_AUTH_VERIFIED) {
        return result;
    }
    const struct bot_key *key = key_find(keys, input.keyid);
    if (key == NULL) {
        return BOT_AUTH_UNKNOWN_KEY;
    }
    unsigned char signature[SIGNATURE_LEN];
    if (!signature_parse(signature_text, input.label, signature)) {
        return BOT_AUTH_MALFORMED;
    }
    struct base_text base = {.len = 0, .is_full = false};
    result = base_format(message, &input, &base);
    if (result != BOT_AUTH_VERIFIED) {
        return result;
    }
    if (!signature_is_valid(key->public_key, signature, &base)) {
        return BOT_AUTH_BAD_SIGNATURE;
    }
    if (keyid != NULL) {
        strcpy(keyid, key->keyid);
    }

    return BOT_AUTH_VERIFIED;
}

/* Returns the bytes of the line that starts at start, without its '\n'. */
static size_t line_measure(const char *start)
{
    const char *end = strchr(start, '\n');

    return end != NULL ? (size_t)(end - start) : strlen(start);
}

/* Cuts the comment and the blanks at the two ends. */
static char *line_trim(char *line)
{
    char *comment = strchr(line, '#');
    if (comment != NULL) {
        *comment = '\0';
    }
    line += strspn(line, BLANKS);
    size_t len = strlen(line);
    while (len > 0 && strchr(BLANKS, line[len - 1]) != NULL) {
        len--;
    }
    line[len] = '\0';

    return line;
}

/* "KEYID BASE64": two words. */
static bool key_parse(char *text, struct bot_key *key)
{
    size_t keyid_len = strcspn(text, BLANKS);
    if (keyid_len > BOT_AUTH_KEYID_MAX) {
        return false;
    }
    memcpy(key->keyid, text, keyid_len);
    key->keyid[keyid_len] = '\0';
    const char *encoded = text + keyid_len + strspn(text + keyid_len, BLANKS);
    if (strlen(encoded) != KEY_BASE64_LEN) {
        return false;
    }

    return base64_decode(encoded, KEY_BASE64_LEN, key->public_key,
                         BOT_AUTH_PUBLIC_KEY_LEN);
}

/* Decodes standard base64 that must give exactly out_len bytes. */
static bool base64_decode(const char *text, size_t len, unsigned char *out,
                          size_t out_len)
{
    if (len == 0 || len % 4 != 0 || len / 4 * 3 > BASE64_DECODED_MAX) {
        return false;
    }
    unsigned char decoded[BASE64_DECODED_MAX];
    int decoded_len =
        EVP_DecodeBlock(decoded, (const unsigned char *)text, (int)len);
    size_t padding = (text[len - 1] == '=') + (text[len - 2] == '=');
    if (decoded_len < 0 || (size_t)decoded_len - padding != out_len) {
        return false;
    }
    memcpy(out, decoded, out_len);

    return true;
}

/* Reads the first member of Signature-Input. */
static bool signature_input_parse(const char *text,
                                  struct signature_input *input)
{
    memset(input, 0, sizeof(*input));
    const char *cursor = text;
    const char *value = NULL;
    size_t value_len = 0;
    if (!dictionary_next(&cursor, input->label, &value, &value_len) ||
        value_len > PARAMS_MAX) {
        return false;
    }
    memcpy(input->params, value, value_len);
    input->params[value_len] = '\0';
    cursor = input->params;
    if (!inner_list_parse(&cursor, input)) {
        return false;
    }
    while (*cursor == ';') {
        char name[LABEL_MAX + 1];
        char param_text[PARAM_TEXT_MAX + 1];
        long long number = 0;
        bool is_number = false;
        if (!param_parse(&cursor, name, param_text, &number, &is_number) ||
            !input_param_set(input, name, param_text, number, is_number)) {
            return false;
        }
    }

    return *cursor == '\0';
}

/* Reads the next member of a Structured Field Dictionary at *cursor: its
   key, and the raw text of its value with the parameters. Moves *cursor
   past the comma. Returns false at the end or on a parse error. */
static bool dictionary_next(const char **cursor, char *key, const char **value,
                            size_t *value_len)
{
    const char *member = *cursor + strspn(*cursor, " \t");
    size_t key_len = strspn(member, KEY_CHARS);
    if (key_len == 0 || key_len > LABEL_MAX || member[key_len] != '=') {
        return false;
    }
    memcpy(key, member, key_len);
    key[key_len] = '\0';
    *value = member + key_len + 1;
    size_t len = member_measure(*value);
    *cursor = *value + len + ((*value)[len] == ',' ? 1 : 0);
    while (len > 0 && strchr(BLANKS, (*value)[len - 1]) != NULL) {
        len--;
    }
    *value_len = len;

    return true;
}

/* Returns the length up to the next comma outside a string and outside
   parentheses. */
static size_t member_measure(const char *text)
{
    size_t len = 0;
    int depth = 0;
    bool in_string = false;
    for (; text[len] != '\0'; len++) {
        if (in_string && text[len] == '\\' && text[len + 1] != '\0') {
            len++;
        } else if (text[len] == '"') {
            in_string = !in_string;
        } else if (!in_string && text[len] == '(') {
            depth++;
        } else if (!in_string && text[len] == ')') {
            depth--;
        } else if (!in_string && depth == 0 && text[len] == ',') {
            break;
        }
    }

    return len;
}

/* RFC 8941 section 4.2.1.2: "(" items separated by spaces ")". Each item
   is a String with parameters. */
static bool inner_list_parse(const char **cursor, struct signature_input *input)
{
    const char *pos = *cursor;
    if (*pos != '(') {
        return false;
    }
    pos++;
    while (true) {
        pos += strspn(pos, " ");
        if (*pos == ')') {
            break;
        }
        char name[COMPONENT_NAME_MAX + 1];
        const char *start = pos;
        if (input->component_count == COMPONENT_MAX ||
            !string_parse(&pos, name, COMPONENT_NAME_MAX) ||
            !params_skip(&pos) || (*pos != ' ' && *pos != ')')) {
            return false;
        }
        input->components[input->component_count] = start;
        input->component_lens[input->component_count] = (size_t)(pos - start);
        input->component_count++;
    }
    *cursor = pos + 1;

    return true;
}

/* RFC 8941 section 4.2.5: a String in quotes, with \" and \\. */
static bool string_parse(const char **cursor, char *out, size_t out_max)
{
    const char *pos = *cursor;
    if (*pos != '"') {
        return false;
    }
    pos++;
    size_t len = 0;
    while (*pos != '"') {
        if (*pos == '\\' && (pos[1] == '"' || pos[1] == '\\')) {
            pos++;
        }
        if (*pos < PRINTABLE_FIRST || *pos > PRINTABLE_LAST || len == out_max) {
            return false;
        }
        out[len++] = *pos++;
    }
    out[len] = '\0';
    *cursor = pos + 1;

    return true;
}

static bool params_skip(const char **cursor)
{
    while (**cursor == ';') {
        char name[LABEL_MAX + 1];
        char text[PARAM_TEXT_MAX + 1];
        long long number = 0;
        bool is_number = false;
        if (!param_parse(cursor, name, text, &number, &is_number)) {
            return false;
        }
    }

    return true;
}

/* ";name=value". The value is a String or an Integer. A name with no
   value is the Boolean true: text is then empty. */
static bool param_parse(const char **cursor, char *name, char *text,
                        long long *number, bool *is_number)
{
    const char *pos = *cursor + 1;
    size_t name_len = strspn(pos, KEY_CHARS);
    if (name_len == 0 || name_len > LABEL_MAX) {
        return false;
    }
    memcpy(name, pos, name_len);
    name[name_len] = '\0';
    pos += name_len;
    text[0] = '\0';
    *is_number = false;
    if (*pos == '=') {
        pos++;
        *is_number = *pos != '"';
        bool parsed = *is_number ? integer_parse(&pos, number)
                                 : string_parse(&pos, text, PARAM_TEXT_MAX);
        if (!parsed) {
            return false;
        }
    }
    *cursor = pos;

    return true;
}

/* RFC 8941 section 4.2.4: an optional "-", then 1 to 15 digits. */
static bool integer_parse(const char **cursor, long long *number)
{
    const char *pos = *cursor;
    const char *digits = *pos == '-' ? pos + 1 : pos;
    size_t digit_count = strspn(digits, "0123456789");
    if (digit_count == 0 || digit_count > INTEGER_DIGITS_MAX) {
        return false;
    }
    *number = strtoll(pos, NULL, 10);
    *cursor = digits + digit_count;

    return true;
}

/* RFC 9421 section 2.3. nonce and tag are read and not checked. */
static bool input_param_set(struct signature_input *input, const char *name,
                            const char *text, long long number, bool is_number)
{
    if (strcmp(name, "created") == 0) {
        input->created = number;
        input->has_created = true;
        return is_number;
    }
    if (strcmp(name, "expires") == 0) {
        input->expires = number;
        input->has_expires = true;
        return is_number;
    }
    if (strcmp(name, "keyid") == 0) {
        if (is_number || strlen(text) > BOT_AUTH_KEYID_MAX) {
            return false;
        }
        strcpy(input->keyid, text);
    } else if (strcmp(name, "alg") == 0) {
        if (is_number) {
            return false;
        }
        strcpy(input->alg, text);
    }

    return true;
}

static enum bot_auth_result input_check(const struct signature_input *input,
                                        double now)
{
    if (input->alg[0] != '\0' && strcmp(input->alg, ALG_ED25519) != 0) {
        return BOT_AUTH_UNSUPPORTED;
    }
    if (!input_covers_authority(input)) {
        return BOT_AUTH_UNSUPPORTED;
    }
    if (!input->has_created || input->keyid[0] == '\0') {
        return BOT_AUTH_MALFORMED;
    }
    if ((double)input->created > now + BOT_AUTH_SKEW_SECONDS) {
        return BOT_AUTH_NOT_YET_VALID;
    }
    long long expires = input->has_expires
                            ? input->expires
                            : input->created + BOT_AUTH_MAX_AGE_SECONDS;
    if (now >= (double)expires) {
        return BOT_AUTH_EXPIRED;
    }

    return BOT_AUTH_VERIFIED;
}

/* The Web Bot Auth draft, section 4.2: the signature covers @authority
   (or @target-uri, which this module does not support). */
static bool input_covers_authority(const struct signature_input *input)
{
    for (size_t i = 0; i < input->component_count; i++) {
        if (input->component_lens[i] == strlen(AUTHORITY_COMPONENT) &&
            memcmp(input->components[i], AUTHORITY_COMPONENT,
                   input->component_lens[i]) == 0) {
            return true;
        }
    }

    return false;
}

static const struct bot_key *key_find(const struct bot_key_set *keys,
                                      const char *keyid)
{
    for (size_t i = 0; i < keys->count; i++) {
        if (strcmp(keys->keys[i].keyid, keyid) == 0) {
            return &keys->keys[i];
        }
    }

    return NULL;
}

/* Signature is a Dictionary of Byte Sequences: label=:BASE64:. */
static bool signature_parse(const char *text, const char *label,
                            unsigned char *signature)
{
    const char *value = NULL;
    size_t value_len = 0;
    if (!member_find(text, label, &value, &value_len) ||
        value_len != SIGNATURE_BASE64_LEN + 2 || value[0] != ':' ||
        value[value_len - 1] != ':') {
        return false;
    }

    return base64_decode(value + 1, SIGNATURE_BASE64_LEN, signature,
                         SIGNATURE_LEN);
}

/* RFC 9421 section 2.5: one line for each component, then the
   @signature-params line with no '\n'. */
static enum bot_auth_result base_format(const struct bot_auth_message *message,
                                        const struct signature_input *input,
                                        struct base_text *base)
{
    for (size_t i = 0; i < input->component_count; i++) {
        enum bot_auth_result result = component_append(
            message, input->components[i], input->component_lens[i], base);
        if (result != BOT_AUTH_VERIFIED) {
            return result;
        }
    }
    base_append_text(base, "\"@signature-params\": ");
    base_append_text(base, input->params);

    return base->is_full ? BOT_AUTH_MALFORMED : BOT_AUTH_VERIFIED;
}

/* Appends "raw: value\n". raw is "name" with an optional ;key="member". */
static enum bot_auth_result
component_append(const struct bot_auth_message *message, const char *raw,
                 size_t raw_len, struct base_text *base)
{
    char text[PARAMS_MAX + 1];
    memcpy(text, raw, raw_len);
    text[raw_len] = '\0';
    const char *cursor = text;
    char name[COMPONENT_NAME_MAX + 1];
    char member[PARAM_TEXT_MAX + 1];
    /* The parse of the inner list checked this string. */
    string_parse(&cursor, name, COMPONENT_NAME_MAX);
    bool has_member = *cursor != '\0';
    if (has_member && (strncmp(cursor, KEY_PARAM, strlen(KEY_PARAM)) != 0 ||
                       name[0] == '@')) {
        return BOT_AUTH_UNSUPPORTED;
    }
    if (has_member) {
        cursor += strlen(KEY_PARAM);
        if (!string_parse(&cursor, member, PARAM_TEXT_MAX) || *cursor != '\0') {
            return BOT_AUTH_UNSUPPORTED;
        }
    }
    base_append(base, raw, raw_len);
    base_append_text(base, ": ");
    if (name[0] == '@') {
        return derived_append(message, name, base);
    }
    const char *value = message->find_header(message->context, name);
    size_t value_len = value != NULL ? strlen(value) : 0;
    if (value == NULL ||
        (has_member && !member_find(value, member, &value, &value_len))) {
        return BOT_AUTH_MALFORMED;
    }
    base_append(base, value, value_len);
    base_append_text(base, "\n");

    return BOT_AUTH_VERIFIED;
}

/* RFC 9421 section 2.2. */
static enum bot_auth_result
derived_append(const struct bot_auth_message *message, const char *name,
               struct base_text *base)
{
    const char *target = message->target;
    size_t path_len = strcspn(target, "?");
    if (strcmp(name, "@method") == 0) {
        base_append_text(base, message->method);
    } else if (strcmp(name, "@authority") == 0) {
        char authority[AUTHORITY_MAX + 1];
        authority_normalize(message->authority, authority);
        base_append_text(base, authority);
    } else if (strcmp(name, "@path") == 0) {
        base_append(base, path_len > 0 ? target : "/",
                    path_len > 0 ? path_len : 1);
    } else if (strcmp(name, "@query") == 0) {
        base_append_text(base,
                         target[path_len] == '?' ? target + path_len : "?");
    } else if (strcmp(name, "@request-target") == 0) {
        base_append_text(base, target);
    } else {
        return BOT_AUTH_UNSUPPORTED;
    }
    base_append_text(base, "\n");

    return BOT_AUTH_VERIFIED;
}

/* RFC 9110 section 4.2.3: the host in lowercase, with no default port. */
static void authority_normalize(const char *authority, char *normalized)
{
    size_t len = strlen(authority);
    if (len > AUTHORITY_MAX) {
        len = AUTHORITY_MAX;
    }
    size_t port_len = strlen(HTTPS_DEFAULT_PORT);
    if (len > port_len &&
        strcmp(authority + len - port_len, HTTPS_DEFAULT_PORT) == 0) {
        len -= port_len;
    }
    for (size_t i = 0; i < len; i++) {
        char letter = authority[i];
        normalized[i] = letter >= 'A' && letter <= 'Z'
                            ? (char)(letter - 'A' + 'a')
                            : letter;
    }
    normalized[len] = '\0';
}

/* Finds the member key of the Dictionary text. The value is the raw text
   of the member. It is the RFC 8941 serialization when the sender
   serialized by RFC 8941. */
static bool member_find(const char *text, const char *key, const char **value,
                        size_t *value_len)
{
    char member_key[LABEL_MAX + 1];
    const char *cursor = text;
    while (dictionary_next(&cursor, member_key, value, value_len)) {
        if (strcmp(member_key, key) == 0) {
            return true;
        }
    }

    return false;
}

static void base_append(struct base_text *base, const char *text, size_t len)
{
    if (len > BASE_MAX - base->len) {
        base->is_full = true;
        return;
    }
    memcpy(base->bytes + base->len, text, len);
    base->len += len;
}

static void base_append_text(struct base_text *base, const char *text)
{
    base_append(base, text, strlen(text));
}

/* Ed25519 signs the message itself, so the digest is NULL (OpenSSL
   EVP_DigestVerify, "Ed25519" in EVP_SIGNATURE-ED25519(7)). */
static bool signature_is_valid(const unsigned char *public_key,
                               const unsigned char *signature,
                               const struct base_text *base)
{
    EVP_PKEY *verify_key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, NULL, public_key, BOT_AUTH_PUBLIC_KEY_LEN);
    EVP_MD_CTX *verify_context = EVP_MD_CTX_new();
    bool is_valid =
        verify_key != NULL && verify_context != NULL &&
        EVP_DigestVerifyInit(verify_context, NULL, NULL, NULL, verify_key) ==
            1 &&
        EVP_DigestVerify(verify_context, signature, SIGNATURE_LEN,
                         (const unsigned char *)base->bytes, base->len) == 1;
    EVP_MD_CTX_free(verify_context);
    EVP_PKEY_free(verify_key);

    return is_valid;
}
