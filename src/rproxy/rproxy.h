#ifndef RPROXY_H
#define RPROXY_H

#include <stdatomic.h>
#include <stdbool.h>

#include "config.h"
#include "forward.h"

/* What all Event loops of the Reverse Proxy share: the settings and the
   decision tables. main.c makes it at the start and frees it after the
   loops end. */

/* --block-key can occur this many times. */
#define BLOCK_KEY_ARG_MAX 16
/* --pow-prefix and --pay-prefix can each occur this many times. */
#define PREFIX_ARG_MAX 8
/* The HMAC key of the pow nonce. RFC 2104 section 3: a key as long as
   the hash output (SHA-256: 32 bytes). */
#define POW_SECRET_LEN 32

/* The key of the rate limit table. */
enum rate_key { RATE_KEY_IP, RATE_KEY_JA4 };

/* How a reply moves from the Origin to the client (SPEC 12, "rproxy
   flags"). The first value is the default. */
enum relay_mode {
    RELAY_REPLY_CAP,    /* read the whole reply, 502 over ORIGIN_REPLY_MAX */
    RELAY_UNBOUNDED,    /* read the whole reply, with no size limit */
    RELAY_CONN_CAP,     /* 503 over relay_conns Origin exchanges */
    RELAY_BACKPRESSURE, /* read the Origin only when relay_buffer has room */
    RELAY_MEMORY_CAP    /* 502 for a new exchange over relay_memory MiB */
};

struct rproxy_config {
    int port;
    const char *cert_file;
    const char *key_file;
    int origin_timeout;
    int client_timeout; /* seconds that rproxy waits for a client */
    int loops;          /* Event loops, one thread each */
    int connections;    /* open connections for each Event loop */
    double rate;
    double burst;
    enum rate_key rate_key;
    int tarpit;
    const char *block_list_file; /* NULL when there is no block list */
    const char *ja4_list_file;   /* NULL when there is no JA4 table */
    int block_time;
    /* Keys that are in the block table from the start, for block_time. */
    const char *block_keys[BLOCK_KEY_ARG_MAX];
    int block_key_count;
    /* Targets that start with one of these need a Proof of work. */
    const char *pow_prefixes[PREFIX_ARG_MAX];
    int pow_prefix_count;
    int pow_bits;
    /* Targets that start with one of these make a crawler pay (402). */
    const char *pay_prefixes[PREFIX_ARG_MAX];
    int pay_prefix_count;
    const char *bot_key_file; /* NULL when there is no Web Bot Auth */
    struct cache_options cache_options;
    enum relay_mode relay_mode;
    int relay_conns;  /* RELAY_CONN_CAP: Origin exchanges in the process */
    int relay_queue;  /* RELAY_CONN_CAP: seconds to wait for a slot, 0: 503 */
    int relay_buffer; /* RELAY_BACKPRESSURE: bytes for each connection */
    int relay_memory; /* RELAY_MEMORY_CAP: MiB of replies in the process */
    /* Each loop writes loop-<N>.txt here: NULL when the stats are off. */
    const char *relay_stats_dir;
};

struct tls_context;
struct rate_limit;
struct ip_list;
struct ja4_table;
struct block_table;
struct behavior;
struct bot_key_set;

struct rproxy {
    struct rproxy_config config;
    struct origin_list list;
    struct tls_context *context;
    struct rate_limit *limit;       /* NULL when the rate limit is off */
    struct cache *cache;            /* NULL when the cache is off */
    struct ip_list *block_list;     /* NULL without --block-list */
    struct ja4_table *fingerprints; /* NULL without --ja4-list */
    struct block_table *blocks;
    struct behavior *counters;
    struct bot_key_set *keys; /* NULL without --bot-key */
    unsigned char pow_secret[POW_SECRET_LEN];
    /* main sets it after SIGTERM or SIGINT. Each loop reads it at least
       once a second. */
    atomic_bool is_stopping;
    /* RELAY_CONN_CAP: the Origin exchanges in progress in all loops. */
    atomic_int origin_exchanges;
    /* RELAY_MEMORY_CAP: the reply bytes that all loops hold. */
    atomic_size_t reply_bytes_held;
};

#endif
