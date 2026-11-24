#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "behavior.h"
#include "block_table.h"
#include "bot_auth.h"
#include "cache.h"
#include "cache_options.h"
#include "config.h"
#include "event_loop.h"
#include "forward.h"
#include "ip_list.h"
#include "ja4_table.h"
#include "pow.h"
#include "rate_limit.h"
#include "rproxy.h"
#include "tls.h"

#define DEFAULT_ORIGIN_TIMEOUT 5
#define DEFAULT_BURST 10.0
#define DEFAULT_TARPIT 10
/* nginx worker_connections in the nginx.conf that nginx ships. */
#define DEFAULT_CONNECTIONS 1024
#define CONNECTIONS_MAX 65536
#define LOOPS_MAX 256
#define RATE_LIMIT_KEY_MAX 4096
/* The same bound as the rate limit table. */
#define BLOCK_TABLE_KEY_MAX 4096
/* fail2ban bantime: 10 minutes. */
#define DEFAULT_BLOCK_TIME 600
#define BLOCK_TIME_MAX 86400
/* fail2ban findtime: 10 minutes. */
#define BEHAVIOR_WINDOW 600
/* The same bound as the rate limit table. */
#define BEHAVIOR_TABLE_KEY_MAX 4096
/* Anubis DefaultDifficulty is 4 leading zero hex digits: 16 bits. */
#define DEFAULT_POW_BITS 16
#define NANOSECONDS_PER_SECOND 1e9
/* SPEC 12, "Values": the Act 3 limit and the Act 4 buffer. */
#define DEFAULT_RELAY_CONNS 20
#define DEFAULT_RELAY_BUFFER 65536
/* One TLS record body, so that one send has bytes to give. */
#define RELAY_BUFFER_MIN 16384
#define RELAY_BUFFER_MAX 67108864

/* One Event loop and the thread that runs it. */
struct loop_thread {
    struct event_loop *loop;
    pthread_t thread;
    int result;
};

static bool rproxy_init_from_args(struct rproxy *rproxy, int argc, char **argv);
static int loops_get_default(void);
static bool option_parse(struct rproxy *rproxy, const char *name,
                         const char *value);
static bool number_parse(const char *text, long min, long max, int *number);
static bool real_parse(const char *text, double min, double *number);
static bool rate_key_parse(const char *text, enum rate_key *rate_key);
static bool relay_mode_parse(const char *text, enum relay_mode *relay_mode);
static bool origin_add(struct origin_list *list, const char *text);
static bool block_key_add(struct rproxy_config *config, const char *key);
static bool prefix_add(const char **prefixes, int *count, const char *prefix);
static bool rproxy_start(struct rproxy *rproxy);
static bool relay_is_valid(const struct rproxy_config *config);
static bool origins_resolve(struct origin_list *list);
static bool defenses_start(struct rproxy *rproxy);
static double clock_get_seconds(void);
static char *file_read_text(const char *path);
static bool block_list_load(struct rproxy *rproxy);
static bool ja4_table_load(struct rproxy *rproxy);
static bool bot_keys_load(struct rproxy *rproxy);
static void rproxy_stop(struct rproxy *rproxy);
static void signals_block(sigset_t *stop_signals);
static int loops_run(struct rproxy *rproxy, const sigset_t *stop_signals);
static int loops_create(struct rproxy *rproxy, struct loop_thread *threads,
                        int count);
static int loops_start(struct loop_thread *threads, int count);
static void *loop_thread_main(void *arg);
static int loops_join(struct loop_thread *threads, int count);

int main(int argc, char **argv)
{
    struct rproxy rproxy;
    if (!rproxy_init_from_args(&rproxy, argc, argv)) {
        fprintf(stderr,
                "usage: ./rproxy --port P --cert FILE --key FILE "
                "--origin HOST:PORT [--origin HOST:PORT ...] "
                "[--origin-timeout S] [--loops N] [--connections N] "
                "[--rate R] [--burst B] "
                "[--rate-key ip|ja4] [--tarpit S] [--cache off|on] "
                "[--cache-admit all|size|second] [--cache-object-max BYTES] "
                "[--cache-evict fifo|lru] [--cache-bytes N] [--cache-ttl S] "
                "[--cache-lock global|striped] [--block-list FILE] "
                "[--ja4-list FILE] [--block-time S] [--block-key KEY ...] "
                "[--pow-prefix PATH ...] [--pow-bits N] "
                "[--pay-prefix PATH ...] [--bot-key FILE] "
                "[--relay unbounded|reply-cap|conn-cap|backpressure] "
                "[--relay-conns N] [--relay-buffer BYTES]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    sigset_t stop_signals;
    signals_block(&stop_signals);
    if (!rproxy_start(&rproxy)) {
        rproxy_stop(&rproxy);
        return 1;
    }
    int result = loops_run(&rproxy, &stop_signals);
    rproxy_stop(&rproxy);

    return result == 0 ? 0 : 1;
}

static bool rproxy_init_from_args(struct rproxy *rproxy, int argc, char **argv)
{
    *rproxy = (struct rproxy){
        .config = {.origin_timeout = DEFAULT_ORIGIN_TIMEOUT,
                   .loops = loops_get_default(),
                   .connections = DEFAULT_CONNECTIONS,
                   .burst = DEFAULT_BURST,
                   .tarpit = DEFAULT_TARPIT,
                   .block_time = DEFAULT_BLOCK_TIME,
                   .pow_bits = DEFAULT_POW_BITS,
                   .relay_conns = DEFAULT_RELAY_CONNS,
                   .relay_buffer = DEFAULT_RELAY_BUFFER},
    };
    cache_options_init(&rproxy->config.cache_options);
    atomic_init(&rproxy->list.next, 0);
    atomic_init(&rproxy->is_stopping, false);
    atomic_init(&rproxy->origin_exchanges, 0);
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc || !option_parse(rproxy, argv[i], argv[i + 1])) {
            return false;
        }
    }
    const struct rproxy_config *config = &rproxy->config;

    return config->port > 0 && config->cert_file != NULL &&
           config->key_file != NULL && rproxy->list.count > 0;
}

/* One Event loop for each online CPU core, as nginx worker_processes
   auto and the Envoy --concurrency default. */
static int loops_get_default(void)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) {
        return 1;
    }

    return cores > LOOPS_MAX ? LOOPS_MAX : (int)cores;
}

static bool option_parse(struct rproxy *rproxy, const char *name,
                         const char *value)
{
    struct rproxy_config *config = &rproxy->config;
    if (strcmp(name, "--port") == 0) {
        return number_parse(value, 1, PORT_MAX, &config->port);
    }
    if (strcmp(name, "--cert") == 0) {
        config->cert_file = value;
        return true;
    }
    if (strcmp(name, "--key") == 0) {
        config->key_file = value;
        return true;
    }
    if (strcmp(name, "--origin") == 0) {
        return origin_add(&rproxy->list, value);
    }
    if (strcmp(name, "--origin-timeout") == 0) {
        return number_parse(value, 1, TIMEOUT_MAX, &config->origin_timeout);
    }
    if (strcmp(name, "--loops") == 0) {
        return number_parse(value, 1, LOOPS_MAX, &config->loops);
    }
    if (strcmp(name, "--connections") == 0) {
        return number_parse(value, 1, CONNECTIONS_MAX, &config->connections);
    }
    if (strcmp(name, "--rate") == 0) {
        return real_parse(value, 0, &config->rate);
    }
    if (strcmp(name, "--burst") == 0) {
        return real_parse(value, 1, &config->burst);
    }
    if (strcmp(name, "--rate-key") == 0) {
        return rate_key_parse(value, &config->rate_key);
    }
    if (strcmp(name, "--tarpit") == 0) {
        return number_parse(value, 0, TIMEOUT_MAX, &config->tarpit);
    }
    if (strcmp(name, "--block-list") == 0) {
        config->block_list_file = value;
        return true;
    }
    if (strcmp(name, "--ja4-list") == 0) {
        config->ja4_list_file = value;
        return true;
    }
    if (strcmp(name, "--block-time") == 0) {
        return number_parse(value, 1, BLOCK_TIME_MAX, &config->block_time);
    }
    if (strcmp(name, "--block-key") == 0) {
        return block_key_add(config, value);
    }
    if (strcmp(name, "--pow-prefix") == 0) {
        return prefix_add(config->pow_prefixes, &config->pow_prefix_count,
                          value);
    }
    if (strcmp(name, "--pow-bits") == 0) {
        return number_parse(value, 0, POW_BITS_MAX, &config->pow_bits);
    }
    if (strcmp(name, "--pay-prefix") == 0) {
        return prefix_add(config->pay_prefixes, &config->pay_prefix_count,
                          value);
    }
    if (strcmp(name, "--bot-key") == 0) {
        config->bot_key_file = value;
        return true;
    }
    if (strcmp(name, "--relay") == 0) {
        return relay_mode_parse(value, &config->relay_mode);
    }
    if (strcmp(name, "--relay-conns") == 0) {
        return number_parse(value, 1, CONNECTIONS_MAX, &config->relay_conns);
    }
    if (strcmp(name, "--relay-buffer") == 0) {
        return number_parse(value, RELAY_BUFFER_MIN, RELAY_BUFFER_MAX,
                            &config->relay_buffer);
    }
    return cache_options_parse(&config->cache_options, name, value);
}

static bool number_parse(const char *text, long min, long max, int *number)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min ||
        value > max) {
        return false;
    }
    *number = (int)value;

    return true;
}

static bool real_parse(const char *text, double min, double *number)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !(value >= min)) {
        return false;
    }
    *number = value;

    return true;
}

static bool rate_key_parse(const char *text, enum rate_key *rate_key)
{
    if (strcmp(text, "ip") == 0) {
        *rate_key = RATE_KEY_IP;
        return true;
    }
    if (strcmp(text, "ja4") == 0) {
        *rate_key = RATE_KEY_JA4;
        return true;
    }

    return false;
}

static bool relay_mode_parse(const char *text, enum relay_mode *relay_mode)
{
    /* In the order of enum relay_mode. */
    static const char *const names[] = {"reply-cap", "unbounded", "conn-cap",
                                        "backpressure"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(text, names[i]) == 0) {
            *relay_mode = (enum relay_mode)i;
            return true;
        }
    }

    return false;
}

static bool origin_add(struct origin_list *list, const char *text)
{
    if (list->count == ORIGIN_MAX ||
        origin_parse(text, &list->origins[list->count]) != 0) {
        return false;
    }
    list->count++;

    return true;
}

static bool block_key_add(struct rproxy_config *config, const char *key)
{
    if (config->block_key_count == BLOCK_KEY_ARG_MAX) {
        return false;
    }
    config->block_keys[config->block_key_count++] = key;

    return true;
}

/* A prefix is a path that starts with "/". */
static bool prefix_add(const char **prefixes, int *count, const char *prefix)
{
    if (*count == PREFIX_ARG_MAX || prefix[0] != '/') {
        return false;
    }
    prefixes[(*count)++] = prefix;

    return true;
}

/* Resolves the Origins, loads the certificate, the block list, and the
   JA4 table, and makes the block table, the rate limit table, and the
   cache. */
static bool rproxy_start(struct rproxy *rproxy)
{
    if (!relay_is_valid(&rproxy->config) || !origins_resolve(&rproxy->list)) {
        return false;
    }
    rproxy->context =
        tls_context_create(rproxy->config.cert_file, rproxy->config.key_file);
    if (rproxy->context == NULL) {
        fprintf(stderr, "rproxy: cannot load %s and %s\n",
                rproxy->config.cert_file, rproxy->config.key_file);
        return false;
    }
    if (!defenses_start(rproxy)) {
        return false;
    }
    if (rproxy->config.rate > 0) {
        struct rate_limit_options options = {.rate = rproxy->config.rate,
                                             .burst = rproxy->config.burst,
                                             .key_max = RATE_LIMIT_KEY_MAX};
        rproxy->limit = rate_limit_create(&options);
        if (rproxy->limit == NULL) {
            fprintf(stderr, "rproxy: rate_limit_create failed\n");
            return false;
        }
    }
    if (rproxy->config.cache_options.is_on) {
        rproxy->cache = cache_create(&rproxy->config.cache_options);
        if (rproxy->cache == NULL) {
            fprintf(stderr, "rproxy: cache_create failed\n");
            return false;
        }
    }

    return true;
}

/* SPEC 12: the cache stores a whole reply, and backpressure never holds
   one. */
static bool relay_is_valid(const struct rproxy_config *config)
{
    if (config->relay_mode == RELAY_BACKPRESSURE &&
        config->cache_options.is_on) {
        fprintf(stderr, "rproxy: --relay backpressure does not work with "
                        "--cache on\n");
        return false;
    }

    return true;
}

/* Once at the start, as nginx resolves the names of an upstream when it
   reads its configuration. An Event loop must not wait for DNS. */
static bool origins_resolve(struct origin_list *list)
{
    for (size_t i = 0; i < list->count; i++) {
        struct origin *origin = &list->origins[i];
        int error = origin_resolve(origin);
        if (error != 0) {
            fprintf(stderr, "rproxy: cannot resolve %s:%s: %s\n", origin->host,
                    origin->port, gai_strerror(error));
            return false;
        }
    }

    return true;
}

/* The files and the block table of the connection Verdict, and the
   behavior counters, the Web Bot Auth keys, and the pow secret of the
   request Verdict. A new secret at each start: a restart makes the old
   challenges invalid. */
static bool defenses_start(struct rproxy *rproxy)
{
    const struct rproxy_config *config = &rproxy->config;
    if (!block_list_load(rproxy) || !ja4_table_load(rproxy) ||
        !bot_keys_load(rproxy)) {
        return false;
    }
    if (RAND_bytes(rproxy->pow_secret, POW_SECRET_LEN) != 1) {
        fprintf(stderr, "rproxy: RAND_bytes failed\n");
        return false;
    }
    rproxy->blocks = block_table_create(BLOCK_TABLE_KEY_MAX);
    if (rproxy->blocks == NULL) {
        fprintf(stderr, "rproxy: block_table_create failed\n");
        return false;
    }
    struct behavior_options options = {.window = BEHAVIOR_WINDOW,
                                       .key_max = BEHAVIOR_TABLE_KEY_MAX};
    rproxy->counters = behavior_create(&options);
    if (rproxy->counters == NULL) {
        fprintf(stderr, "rproxy: behavior_create failed\n");
        return false;
    }
    double now = clock_get_seconds();
    for (int i = 0; i < config->block_key_count; i++) {
        if (!block_table_add(rproxy->blocks, config->block_keys[i],
                             now + config->block_time, now)) {
            fprintf(stderr, "rproxy: bad --block-key %s\n",
                    config->block_keys[i]);
            return false;
        }
    }

    return true;
}

static double clock_get_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (double)now.tv_sec + (double)now.tv_nsec / NANOSECONDS_PER_SECOND;
}

/* Returns the whole file as a malloc string, or NULL. */
static char *file_read_text(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    char *text = NULL;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
    }
    if (size >= 0 && fseek(file, 0, SEEK_SET) == 0) {
        text = malloc((size_t)size + 1);
    }
    if (text != NULL) {
        size_t len = fread(text, 1, (size_t)size, file);
        text[len] = '\0';
    }
    fclose(file);

    return text;
}

static bool block_list_load(struct rproxy *rproxy)
{
    const char *path = rproxy->config.block_list_file;
    if (path == NULL) {
        return true;
    }
    char *text = file_read_text(path);
    rproxy->block_list = malloc(sizeof(*rproxy->block_list));
    if (text == NULL || rproxy->block_list == NULL) {
        fprintf(stderr, "rproxy: cannot read %s\n", path);
        free(text);
        return false;
    }
    size_t bad_line = ip_list_parse(text, rproxy->block_list);
    free(text);
    if (bad_line != 0) {
        fprintf(stderr, "rproxy: %s line %zu: bad CIDR range\n", path,
                bad_line);
    }

    return bad_line == 0;
}

static bool ja4_table_load(struct rproxy *rproxy)
{
    const char *path = rproxy->config.ja4_list_file;
    if (path == NULL) {
        return true;
    }
    char *text = file_read_text(path);
    rproxy->fingerprints = malloc(sizeof(*rproxy->fingerprints));
    if (text == NULL || rproxy->fingerprints == NULL) {
        fprintf(stderr, "rproxy: cannot read %s\n", path);
        free(text);
        return false;
    }
    size_t bad_line = ja4_table_parse(text, rproxy->fingerprints);
    free(text);
    if (bad_line != 0) {
        fprintf(stderr, "rproxy: %s line %zu: bad \"JA4 LABEL\" line\n", path,
                bad_line);
    }

    return bad_line == 0;
}

static bool bot_keys_load(struct rproxy *rproxy)
{
    const char *path = rproxy->config.bot_key_file;
    if (path == NULL) {
        return true;
    }
    char *text = file_read_text(path);
    rproxy->keys = malloc(sizeof(*rproxy->keys));
    if (text == NULL || rproxy->keys == NULL) {
        fprintf(stderr, "rproxy: cannot read %s\n", path);
        free(text);
        return false;
    }
    size_t bad_line = bot_auth_parse_keys(text, rproxy->keys);
    free(text);
    if (bad_line != 0) {
        fprintf(stderr, "rproxy: %s line %zu: bad \"KEYID BASE64\" line\n",
                path, bad_line);
    }

    return bad_line == 0;
}

static void rproxy_stop(struct rproxy *rproxy)
{
    if (rproxy->cache != NULL) {
        cache_report(rproxy->cache);
        cache_destroy(rproxy->cache);
    }
    if (rproxy->limit != NULL) {
        rate_limit_destroy(rproxy->limit);
    }
    block_table_destroy(rproxy->blocks);
    behavior_destroy(rproxy->counters);
    free(rproxy->keys);
    free(rproxy->fingerprints);
    free(rproxy->block_list);
    tls_context_destroy(rproxy->context);
}

/* The main thread takes SIGINT and SIGTERM with sigwait, so no signal
   handler runs. The loop threads inherit the mask. A write to a closed
   socket returns EPIPE instead of a SIGPIPE. */
static void signals_block(sigset_t *stop_signals)
{
    sigemptyset(stop_signals);
    sigaddset(stop_signals, SIGINT);
    sigaddset(stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, stop_signals, NULL);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

/* Each loop thread serves its own connections. The main thread only waits
   for a stop signal, sets the flag, and waits for the loops to end. */
static int loops_run(struct rproxy *rproxy, const sigset_t *stop_signals)
{
    int count = rproxy->config.loops;
    struct loop_thread *threads = calloc((size_t)count, sizeof(*threads));
    if (threads == NULL) {
        return -1;
    }
    if (!event_loop_is_port_free(rproxy->config.port)) {
        fprintf(stderr, "rproxy: port %d is in use\n", rproxy->config.port);
        free(threads);
        return -1;
    }
    int created = loops_create(rproxy, threads, count);
    int started = created == count ? loops_start(threads, count) : 0;
    if (started == count) {
        printf("listening on port %d\n", rproxy->config.port);
        int signal_number = 0;
        sigwait(stop_signals, &signal_number);
    }
    atomic_store(&rproxy->is_stopping, true);
    int result = loops_join(threads, started);
    for (int i = 0; i < created; i++) {
        event_loop_destroy(threads[i].loop);
    }
    free(threads);

    return started == count ? result : -1;
}

/* Returns the count of loops made. All listen sockets exist before the
   first loop runs. */
static int loops_create(struct rproxy *rproxy, struct loop_thread *threads,
                        int count)
{
    for (int i = 0; i < count; i++) {
        threads[i].loop = event_loop_create(rproxy);
        if (threads[i].loop == NULL) {
            return i;
        }
    }

    return count;
}

/* Returns the count of threads started. */
static int loops_start(struct loop_thread *threads, int count)
{
    for (int i = 0; i < count; i++) {
        if (pthread_create(&threads[i].thread, NULL, loop_thread_main,
                           &threads[i]) != 0) {
            return i;
        }
    }

    return count;
}

/* A loop that fails stops the process: the signal wakes sigwait in the
   main thread. */
static void *loop_thread_main(void *arg)
{
    struct loop_thread *thread = arg;
    thread->result = event_loop_run(thread->loop);
    if (thread->result != 0) {
        kill(getpid(), SIGTERM);
    }

    return NULL;
}

/* Returns 0, or -1 if a loop failed. */
static int loops_join(struct loop_thread *threads, int count)
{
    int result = 0;
    for (int i = 0; i < count; i++) {
        pthread_join(threads[i].thread, NULL);
        if (threads[i].result != 0) {
            result = -1;
        }
    }

    return result;
}
