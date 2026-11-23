#include "ip_list.h"

#include <arpa/inet.h>
#include <string.h>

/* The longest line that can hold a range: an IPv6 address, "/128", and
   room for spaces and a comment. Longer lines are bad. */
#define LINE_MAX_LEN 255
#define IPV4_BYTES 4
#define BITS_PER_BYTE 8
#define BITS_DIGITS_MAX 3
#define DECIMAL_BASE 10
/* ::ffff:a.b.c.d (RFC 4291 section 2.5.5.2): 10 zero bytes, 2 0xff bytes,
   then the IPv4 address. */
#define MAPPED_PREFIX_LEN 12
#define MAPPED_FF_INDEX 10

static size_t line_copy(const char *start, char *line);
static char *line_trim(char *line);
static bool range_parse(char *text, struct ip_range *range);
static bool address_parse(const char *text, struct ip_range *range);
static bool bits_parse(const char *text, int max, int *bits);
static void range_mask(struct ip_range *range);
static bool range_has(const struct ip_range *range, const unsigned char *bytes);

size_t ip_list_parse(const char *text, struct ip_list *block_list)
{
    char line[LINE_MAX_LEN + 2];
    size_t line_number = 0;
    block_list->count = 0;
    while (*text != '\0') {
        line_number++;
        size_t len = line_copy(text, line);
        text += len;
        if (len > LINE_MAX_LEN + 1) {
            return line_number;
        }
        char *range_text = line_trim(line);
        if (*range_text == '\0') {
            continue;
        }
        if (block_list->count == IP_LIST_RANGE_MAX ||
            !range_parse(range_text, &block_list->ranges[block_list->count])) {
            return line_number;
        }
        block_list->count++;
    }

    return 0;
}

bool ip_list_has_address(const struct ip_list *block_list, const char *address)
{
    struct ip_range client;
    if (!address_parse(address, &client)) {
        return false;
    }
    bool is_mapped =
        client.is_ipv6 && memcmp(client.prefix, "\0\0\0\0\0\0\0\0\0\0\xff\xff",
                                 MAPPED_PREFIX_LEN) == 0;
    if (is_mapped) {
        client.is_ipv6 = false;
        memmove(client.prefix, client.prefix + MAPPED_PREFIX_LEN, IPV4_BYTES);
    }
    for (size_t i = 0; i < block_list->count; i++) {
        const struct ip_range *range = &block_list->ranges[i];
        if (range->is_ipv6 == client.is_ipv6 &&
            range_has(range, client.prefix)) {
            return true;
        }
    }

    return false;
}

/* Copies the line that starts at start into line, which has
   LINE_MAX_LEN + 2 bytes, and cuts it there if it is longer. Returns the
   bytes of the line in the text, with its '\n'. */
static size_t line_copy(const char *start, char *line)
{
    const char *end = strchr(start, '\n');
    size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
    size_t kept = len <= LINE_MAX_LEN ? len : LINE_MAX_LEN + 1;
    memcpy(line, start, kept);
    line[kept] = '\0';

    return end != NULL ? len + 1 : len;
}

/* Cuts the comment and the spaces, tabs, and '\r' at the two ends. */
static char *line_trim(char *line)
{
    char *comment = strchr(line, '#');
    if (comment != NULL) {
        *comment = '\0';
    }
    line += strspn(line, " \t\r");
    size_t len = strlen(line);
    while (len > 0 && strchr(" \t\r", line[len - 1]) != NULL) {
        len--;
    }
    line[len] = '\0';

    return line;
}

/* "ADDRESS" or "ADDRESS/BITS", with no space inside. */
static bool range_parse(char *text, struct ip_range *range)
{
    char *slash = strchr(text, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    if (!address_parse(text, range)) {
        return false;
    }
    int max = range->is_ipv6 ? IP_LIST_ADDRESS_BYTES * BITS_PER_BYTE
                             : IPV4_BYTES * BITS_PER_BYTE;
    range->prefix_bits = max;
    if (slash != NULL && !bits_parse(slash + 1, max, &range->prefix_bits)) {
        return false;
    }
    range_mask(range);

    return true;
}

static bool address_parse(const char *text, struct ip_range *range)
{
    memset(range, 0, sizeof(*range));
    if (inet_pton(AF_INET, text, range->prefix) == 1) {
        return true;
    }
    range->is_ipv6 = true;

    return inet_pton(AF_INET6, text, range->prefix) == 1;
}

/* Only decimal digits, at most max. */
static bool bits_parse(const char *text, int max, int *bits)
{
    size_t len = strlen(text);
    if (len == 0 || len > BITS_DIGITS_MAX ||
        strspn(text, "0123456789") != len) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < len; i++) {
        value = value * DECIMAL_BASE + (text[i] - '0');
    }
    *bits = value;

    return value <= max;
}

/* Sets the bits after the prefix to zero, so that a match can compare
   bytes. */
static void range_mask(struct ip_range *range)
{
    for (int i = 0; i < IP_LIST_ADDRESS_BYTES; i++) {
        int kept = range->prefix_bits - i * BITS_PER_BYTE;
        if (kept <= 0) {
            range->prefix[i] = 0;
        } else if (kept < BITS_PER_BYTE) {
            range->prefix[i] &= (unsigned char)(0xff << (BITS_PER_BYTE - kept));
        }
    }
}

static bool range_has(const struct ip_range *range, const unsigned char *bytes)
{
    int whole = range->prefix_bits / BITS_PER_BYTE;
    int rest = range->prefix_bits % BITS_PER_BYTE;
    if (memcmp(range->prefix, bytes, (size_t)whole) != 0) {
        return false;
    }
    if (rest == 0) {
        return true;
    }
    unsigned char mask = (unsigned char)(0xff << (BITS_PER_BYTE - rest));

    return (bytes[whole] & mask) == range->prefix[whole];
}
