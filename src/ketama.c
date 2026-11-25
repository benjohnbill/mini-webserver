#include "handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ketama uses little-endian MD5 words. Both supported key formats
   (an IPv4 address and IPv4:port-replica) fit in one 64-byte MD5 block. */
static void md5_key(const char *key, uint32_t digest[4])
{
    static const uint32_t constants[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const unsigned shifts[4][4] = {
        {7,12,17,22}, {5,9,14,20}, {4,11,16,23}, {6,10,15,21}
    };
    size_t len = strlen(key);
    uint32_t block[16] = {0};
    for (size_t i = 0; i < len; i++)
        block[i / 4] |= (uint32_t)(unsigned char)key[i] << (8 * (i % 4));
    block[len / 4] |= UINT32_C(0x80) << (8 * (len % 4));
    block[14] = (uint32_t)len * 8;

    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    for (unsigned i = 0; i < 64; i++) {
        uint32_t f;
        unsigned word;
        if (i < 16) {
            f = (b & c) | (~b & d);
            word = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            word = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            word = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            word = (7 * i) % 16;
        }
        uint32_t sum = a + f + constants[i] + block[word];
        unsigned shift = shifts[i / 16][i % 4];
        a = d;
        d = c;
        c = b;
        b += (sum << shift) | (sum >> (32 - shift));
    }
    digest[0] = a + UINT32_C(0x67452301);
    digest[1] = b + UINT32_C(0xefcdab89);
    digest[2] = c + UINT32_C(0x98badcfe);
    digest[3] = d + UINT32_C(0x10325476);
}

static int endpoint_order(const void *left, const void *right)
{
    const Backend *a = left, *b = right;
    int by_ip = strcmp(a->ip, b->ip);
    return by_ip != 0 ? by_ip : (int)a->port - (int)b->port;
}

static int point_order(const void *left, const void *right)
{
    const RingPoint *a = left, *b = right;
    if (a->hash != b->hash)
        return a->hash < b->hash ? -1 : 1;
    return (int)a->backend - (int)b->backend;
}

bool ketama_build(Route *route)
{
    route->ring = malloc(route->count * KETAMA_POINTS * sizeof *route->ring);
    if (!route->ring)
        return false;
    /* Canonical order also breaks ties between identical ring positions. */
    qsort(route->backends, route->count, sizeof route->backends[0], endpoint_order);
    size_t n = 0;
    for (unsigned i = 0; i < route->count; i++) {
        const Backend *backend = &route->backends[i];
        for (unsigned replica = 0; replica < KETAMA_POINTS / 4; replica++) {
            char name[40];
            snprintf(name, sizeof name, "%s:%u-%u", backend->ip, (unsigned)backend->port, replica);
            uint32_t digest[4];
            md5_key(name, digest);
            for (unsigned j = 0; j < 4; j++)
                route->ring[n++] = (RingPoint){digest[j], i};
        }
    }
    qsort(route->ring, n, sizeof route->ring[0], point_order);
    return true;
}

const Backend *ketama_select(const Route *route, struct in_addr peer)
{
    if (route->count == 1)
        return &route->backends[0];
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer, client_ip, sizeof client_ip);
    uint32_t digest[4];
    md5_key(client_ip, digest);
    size_t end = route->count * KETAMA_POINTS;
    size_t lo = 0, hi = end;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (route->ring[mid].hash < digest[0])
            lo = mid + 1;
        else
            hi = mid;
    }
    return &route->backends[route->ring[lo == end ? 0 : lo].backend];
}
