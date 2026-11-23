#ifndef IP_LIST_H
#define IP_LIST_H

#include <stdbool.h>
#include <stddef.h>

/* Decision module of the Reverse Proxy: the block list, a list of CIDR
   ranges (IPv4 and IPv6). No file I/O: main.c reads the file and gives
   the text. */

#define IP_LIST_RANGE_MAX 1024
#define IP_LIST_ADDRESS_BYTES 16

/* One range: the first bits of an address. An IPv4 range keeps 4 bytes. */
struct ip_range {
    bool is_ipv6;
    unsigned char prefix[IP_LIST_ADDRESS_BYTES];
    int prefix_bits;
};

struct ip_list {
    struct ip_range ranges[IP_LIST_RANGE_MAX];
    size_t count;
};

/* Reads text into block_list. One range on each line: "ADDRESS/BITS" or
   "ADDRESS" (all bits). "#" starts a comment to the end of the line. Blank
   lines are skipped. Bits after the prefix in ADDRESS are ignored, as in
   nginx. Returns 0, or the number (from 1) of the first bad line. A line
   after IP_LIST_RANGE_MAX ranges is bad. */
size_t ip_list_parse(const char *text, struct ip_list *block_list);

/* Asks if address (the text of an IPv4 or IPv6 address) is in a range of
   block_list. An IPv4-mapped IPv6 address (::ffff:a.b.c.d) matches the IPv4
   ranges. Text that is not an address matches nothing. */
bool ip_list_has_address(const struct ip_list *block_list, const char *address);

#endif
