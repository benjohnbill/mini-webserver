#include "client_hello.h"

#include <stdbool.h>
#include <string.h>

/* RFC 8446 section 5.1: a plaintext record holds at most 2^14 bytes. */
#define RECORD_HEADER_LEN 5
#define RECORD_BODY_MAX 16384
#define RECORD_TYPE_HANDSHAKE 0x16
#define HANDSHAKE_TYPE_CLIENT_HELLO 1
#define RANDOM_LEN 32
#define EXTENSION_SIGNATURE_ALGORITHMS 0x000d
#define EXTENSION_ALPN 0x0010
#define EXTENSION_SUPPORTED_VERSIONS 0x002b

/* A read position in one container of the input. A read past `end`
   breaks the container: INVALID. A read inside `end` but past the bytes
   that came in so far: INCOMPLETE. The first failure is kept in *status,
   which all cursors of one parse share. After it, reads give 0. */
struct cursor {
    const unsigned char *bytes;
    size_t len;
    size_t pos;
    size_t end;
    enum client_hello_status *status;
};

static bool cursor_take(struct cursor *cursor, size_t n,
                        const unsigned char **start);
static unsigned cursor_take_uint(struct cursor *cursor, size_t width);
static struct cursor cursor_take_vector(struct cursor *cursor,
                                        size_t length_width);
static bool cursor_has_more(const struct cursor *cursor);
static void cursor_fail(struct cursor *cursor);
static void hello_parse_header(struct cursor *cursor);
static void hello_parse_ciphers(struct cursor *cursor,
                                struct client_hello *hello);
static void hello_parse_extensions(struct cursor *cursor,
                                   struct client_hello *hello);
static void hello_parse_extension(struct cursor *data, unsigned type,
                                  struct client_hello *hello);
static size_t hello_parse_u16_list(struct cursor *list, uint16_t *values,
                                   size_t max);
static void hello_parse_alpn(struct cursor *data, struct client_hello *hello);

enum client_hello_status client_hello_parse(const unsigned char *bytes,
                                            size_t len,
                                            struct client_hello *hello)
{
    enum client_hello_status status = CLIENT_HELLO_OK;
    struct cursor input = {bytes, len, 0, RECORD_HEADER_LEN + RECORD_BODY_MAX,
                           &status};
    *hello = (struct client_hello){0};

    hello_parse_header(&input);
    struct cursor record = cursor_take_vector(&input, 2);
    if (cursor_take_uint(&record, 1) != HANDSHAKE_TYPE_CLIENT_HELLO) {
        cursor_fail(&record);
    }
    struct cursor body = cursor_take_vector(&record, 3);
    hello->legacy_version = (uint16_t)cursor_take_uint(&body, 2);
    cursor_take(&body, RANDOM_LEN, NULL);
    cursor_take_vector(&body, 1); /* legacy_session_id */
    hello_parse_ciphers(&body, hello);
    cursor_take_vector(&body, 1); /* legacy_compression_methods */
    /* TLS 1.2 lets a ClientHello end before the extension block. */
    if (cursor_has_more(&body)) {
        hello_parse_extensions(&body, hello);
    }
    /* The data of an extension that is not read must also be here. */
    cursor_take(&record, record.end - record.pos, NULL);

    return status;
}

/* Reads n bytes and sets *start to them, if start is not NULL. */
static bool cursor_take(struct cursor *cursor, size_t n,
                        const unsigned char **start)
{
    if (*cursor->status != CLIENT_HELLO_OK) {
        return false;
    }
    if (n > cursor->end - cursor->pos) {
        *cursor->status = CLIENT_HELLO_INVALID;
        return false;
    }
    if (cursor->pos + n > cursor->len) {
        *cursor->status = CLIENT_HELLO_INCOMPLETE;
        return false;
    }
    if (start != NULL) {
        *start = cursor->bytes + cursor->pos;
    }
    cursor->pos += n;

    return true;
}

/* Reads a big-endian number of 1 to 3 bytes. */
static unsigned cursor_take_uint(struct cursor *cursor, size_t width)
{
    const unsigned char *start = NULL;
    if (!cursor_take(cursor, width, &start)) {
        return 0;
    }
    unsigned value = 0;
    for (size_t i = 0; i < width; i++) {
        value = (value << 8) | start[i];
    }

    return value;
}

/* Reads a length of length_width bytes and gives a cursor over the bytes
   that it counts. The caller's cursor moves past them. The length is
   checked against the container before the bytes come in. */
static struct cursor cursor_take_vector(struct cursor *cursor,
                                        size_t length_width)
{
    size_t vector_len = cursor_take_uint(cursor, length_width);
    struct cursor vector = *cursor;
    vector.end = vector.pos;
    if (*cursor->status != CLIENT_HELLO_OK) {
        return vector;
    }
    if (vector_len > cursor->end - cursor->pos) {
        *cursor->status = CLIENT_HELLO_INVALID;
        return vector;
    }
    vector.end = vector.pos + vector_len;
    cursor->pos = vector.end;

    return vector;
}

static bool cursor_has_more(const struct cursor *cursor)
{
    return *cursor->status == CLIENT_HELLO_OK && cursor->pos < cursor->end;
}

static void cursor_fail(struct cursor *cursor)
{
    if (*cursor->status == CLIENT_HELLO_OK) {
        *cursor->status = CLIENT_HELLO_INVALID;
    }
}

/* The content type and the legacy record version. The caller reads the
   record length as a vector: the input cursor ends RECORD_BODY_MAX bytes
   after the header, so a longer record is INVALID. */
static void hello_parse_header(struct cursor *cursor)
{
    if (cursor_take_uint(cursor, 1) != RECORD_TYPE_HANDSHAKE) {
        cursor_fail(cursor);
    }
    cursor_take_uint(cursor, 2);
}

static void hello_parse_ciphers(struct cursor *cursor,
                                struct client_hello *hello)
{
    struct cursor list = cursor_take_vector(cursor, 2);
    if ((list.end - list.pos) % 2 != 0) {
        cursor_fail(cursor);
    }
    hello->cipher_count =
        hello_parse_u16_list(&list, hello->ciphers, CLIENT_HELLO_CIPHER_MAX);
}

static void hello_parse_extensions(struct cursor *cursor,
                                   struct client_hello *hello)
{
    struct cursor block = cursor_take_vector(cursor, 2);
    while (cursor_has_more(&block)) {
        if (hello->extension_count == CLIENT_HELLO_EXTENSION_MAX) {
            cursor_fail(&block);
            return;
        }
        unsigned type = cursor_take_uint(&block, 2);
        struct cursor data = cursor_take_vector(&block, 2);
        if (*block.status != CLIENT_HELLO_OK) {
            return;
        }
        hello->extensions[hello->extension_count++] = (uint16_t)type;
        hello_parse_extension(&data, type, hello);
    }
}

static void hello_parse_extension(struct cursor *data, unsigned type,
                                  struct client_hello *hello)
{
    if (type == EXTENSION_SUPPORTED_VERSIONS) {
        struct cursor list = cursor_take_vector(data, 1);
        hello->version_count = hello_parse_u16_list(&list, hello->versions,
                                                    CLIENT_HELLO_VERSION_MAX);
    } else if (type == EXTENSION_SIGNATURE_ALGORITHMS) {
        struct cursor list = cursor_take_vector(data, 2);
        hello->signature_count = hello_parse_u16_list(
            &list, hello->signatures, CLIENT_HELLO_SIGNATURE_MAX);
    } else if (type == EXTENSION_ALPN) {
        hello_parse_alpn(data, hello);
    }
}

/* Reads 2-byte values until the list ends. An odd last byte fails as a
   read past the end. Returns the count of values read. */
static size_t hello_parse_u16_list(struct cursor *list, uint16_t *values,
                                   size_t max)
{
    if ((list->end - list->pos) / 2 > max) {
        cursor_fail(list);
        return 0;
    }
    size_t count = 0;
    while (cursor_has_more(list)) {
        unsigned value = cursor_take_uint(list, 2);
        if (*list->status == CLIENT_HELLO_OK) {
            values[count++] = (uint16_t)value;
        }
    }

    return count;
}

/* RFC 7301 section 3.1: a 2-byte list length, then names with a 1-byte
   length each. Only the first name is kept. */
static void hello_parse_alpn(struct cursor *data, struct client_hello *hello)
{
    struct cursor list = cursor_take_vector(data, 2);
    struct cursor name = cursor_take_vector(&list, 1);
    const unsigned char *start = NULL;
    size_t name_len = name.end - name.pos;
    if (cursor_take(&name, name_len, &start) && name_len > 0) {
        memcpy(hello->alpn, start, name_len);
        hello->alpn_len = name_len;
    }
}
