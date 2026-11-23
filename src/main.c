#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cache.h"
#include "config.h"
#include "model.h"

#define names_count(names) ((int)(sizeof(names) / sizeof((names)[0])))

volatile sig_atomic_t stop_requested = 0;

static bool config_init_from_args(struct config *config, int argc, char **argv);
static bool number_parse(const char *text, long min, long max, int *number);
static bool option_parse(struct config *config, const char *name, char *value);
static bool model_is_known(const char *model);
static bool upstream_parse(struct config *config, char *value);
static bool cache_option_parse(struct cache_options *options, const char *name,
                               const char *value);
static int name_find(const char *value, const char *const *names, int count);
static void signals_install(void);
static void stop_signal_handle(int signal_number);
static int listen_socket_open(int port);
static void listen_socket_bind(int listen_fd, int port);
static void socket_addr_init(struct sockaddr_in *addr, int port);
static int model_run(int listen_fd, const struct config *config);
static int model_result_report(const char *model, int result);

int main(int argc, char **argv)
{
    struct config config;
    if (!config_init_from_args(&config, argc, argv)) {
        fprintf(stderr,
                "usage: ./server <port> <root> [--model "
                "iter|fork|pool|epoll] [--workers N] [--timeout S] [--log] "
                "[--upstream HOST:PORT] [--delay MS] [--cache off|on] "
                "[--cache-admit all|size|second] [--cache-object-max BYTES] "
                "[--cache-evict fifo|lru] [--cache-bytes N] [--cache-ttl S] "
                "[--cache-lock global|striped] "
                "[--cache-share private|shared]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signals_install();

    int listen_fd = listen_socket_open(config.port);
    if (config.cache_options.is_on) {
        config.cache = cache_create(&config.cache_options);
        if (config.cache == NULL) {
            fprintf(stderr, "server: cache_create failed\n");
            return 1;
        }
    }
    int result = model_run(listen_fd, &config);
    close(listen_fd);
    if (config.cache != NULL) {
        cache_report(config.cache);
        cache_destroy(config.cache);
    }

    return model_result_report(config.model, result);
}

static bool config_init_from_args(struct config *config, int argc, char **argv)
{
    *config = (struct config){
        .model = DEFAULT_MODEL,
        .workers = DEFAULT_WORKERS,
        .timeout = DEFAULT_TIMEOUT,
        .log = false,
        .upstream_host = NULL,
        .upstream_port = NULL,
        .delay_ms = 0,
        .cache_options = {.is_on = false,
                          .admit = CACHE_ADMIT_ALL,
                          .evict = CACHE_EVICT_FIFO,
                          .lock = CACHE_LOCK_GLOBAL,
                          .share = CACHE_SHARE_PRIVATE,
                          .bytes = DEFAULT_CACHE_BYTES,
                          .object_max = DEFAULT_CACHE_OBJECT_MAX,
                          .ttl = 0},
        .cache = NULL,
    };
    if (argc < 3 || !number_parse(argv[1], 1, PORT_MAX, &config->port)) {
        return false;
    }
    config->root = argv[2];

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) {
            config->log = true;
        } else if (i + 1 < argc && option_parse(config, argv[i], argv[i + 1])) {
            i++;
        } else {
            return false;
        }
    }

    return true;
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

static bool option_parse(struct config *config, const char *name, char *value)
{
    if (strcmp(name, "--model") == 0) {
        config->model = value;
        return model_is_known(value);
    }
    if (strcmp(name, "--workers") == 0) {
        return number_parse(value, 1, WORKERS_MAX, &config->workers);
    }
    if (strcmp(name, "--timeout") == 0) {
        return number_parse(value, 1, TIMEOUT_MAX, &config->timeout);
    }
    if (strcmp(name, "--upstream") == 0) {
        return upstream_parse(config, value);
    }
    if (strcmp(name, "--delay") == 0) {
        return number_parse(value, 0, DELAY_MAX, &config->delay_ms);
    }

    return cache_option_parse(&config->cache_options, name, value);
}

static bool model_is_known(const char *model)
{
    return strcmp(model, "iter") == 0 || strcmp(model, "fork") == 0 ||
           strcmp(model, "pool") == 0 || strcmp(model, "epoll") == 0;
}

/* "127.0.0.1:9091": splits value in place at the last ':'. */
static bool upstream_parse(struct config *config, char *value)
{
    char *colon = strrchr(value, ':');
    int port = 0;
    if (colon == NULL || colon == value ||
        !number_parse(colon + 1, 1, PORT_MAX, &port)) {
        return false;
    }
    *colon = '\0';
    config->upstream_host = value;
    config->upstream_port = colon + 1;

    return true;
}

/* Each names table is in the order of its enum in config.h. */
static bool cache_option_parse(struct cache_options *options, const char *name,
                               const char *value)
{
    static const char *const on_names[] = {"off", "on"};
    static const char *const admit_names[] = {"all", "size", "second"};
    static const char *const evict_names[] = {"fifo", "lru"};
    static const char *const lock_names[] = {"global", "striped"};
    static const char *const share_names[] = {"private", "shared"};
    int index = -1;
    if (strcmp(name, "--cache") == 0) {
        index = name_find(value, on_names, names_count(on_names));
        options->is_on = index == 1;
    } else if (strcmp(name, "--cache-admit") == 0) {
        index = name_find(value, admit_names, names_count(admit_names));
        options->admit = (enum cache_admit)index;
    } else if (strcmp(name, "--cache-evict") == 0) {
        index = name_find(value, evict_names, names_count(evict_names));
        options->evict = (enum cache_evict)index;
    } else if (strcmp(name, "--cache-lock") == 0) {
        index = name_find(value, lock_names, names_count(lock_names));
        options->lock = (enum cache_lock)index;
    } else if (strcmp(name, "--cache-share") == 0) {
        index = name_find(value, share_names, names_count(share_names));
        options->share = (enum cache_share)index;
    } else if (strcmp(name, "--cache-bytes") == 0) {
        return number_parse(value, 1, CACHE_BYTES_MAX, &options->bytes);
    } else if (strcmp(name, "--cache-object-max") == 0) {
        return number_parse(value, 1, CACHE_BYTES_MAX, &options->object_max);
    } else if (strcmp(name, "--cache-ttl") == 0) {
        return number_parse(value, 0, CACHE_TTL_MAX, &options->ttl);
    }

    return index >= 0;
}

/* Returns the index of value in names, or -1. */
static int name_find(const char *value, const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(value, names[i]) == 0) {
            return i;
        }
    }

    return -1;
}

/* sa_flags = 0, not signal(): with -D_POSIX_C_SOURCE, signal() keeps the
   handler for one signal only. No SA_RESTART, so accept and epoll_wait
   return EINTR and the loops see the flag. */
static void signals_install(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = stop_signal_handle;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

static void stop_signal_handle(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int listen_socket_open(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    listen_socket_bind(listen_fd, port);
    printf("listening on port %d\n", port);
    return listen_fd;
}

static void listen_socket_bind(int listen_fd, int port)
{
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
        0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    socket_addr_init(&addr, port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
}

static void socket_addr_init(struct sockaddr_in *addr, int port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_ANY);
    addr->sin_port = htons((uint16_t)port);
}

static int model_run(int listen_fd, const struct config *config)
{
    if (strcmp(config->model, "iter") == 0) {
        return model_iter_run(listen_fd, config);
    }
    if (strcmp(config->model, "fork") == 0) {
        return model_fork_run(listen_fd, config);
    }
    if (strcmp(config->model, "epoll") == 0) {
        return model_epoll_run(listen_fd, config);
    }

    return model_pool_run(listen_fd, config);
}

/* Returns the exit code. A slot returns -1, an error returns 1. */
static int model_result_report(const char *model, int result)
{
    if (result == -1) {
        fprintf(stderr, "server: model %s: SLOT not written (SPEC §8)\n",
                model);
        return 1;
    }
    if (result != 0) {
        fprintf(stderr, "server: model %s: failed\n", model);
        return 1;
    }

    return 0;
}
