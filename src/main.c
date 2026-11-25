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

#include "config.h"
#include "model.h"

volatile sig_atomic_t stop_requested = 0;

static bool config_init_from_args(struct config *config, int argc, char **argv);
static bool number_parse(const char *text, long min, long max, int *number);
static bool option_parse(struct config *config, const char *name, char *value);
static bool model_is_known(const char *model);
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
                "iter|fork|pool|epoll|epoll-pool] [--workers N] [--timeout S] "
                "[--log] [--delay MS] [--work MS] [--stats FILE]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signals_install();

    int listen_fd = listen_socket_open(config.port);
    int result = model_run(listen_fd, &config);
    close(listen_fd);

    return model_result_report(config.model, result);
}

static bool config_init_from_args(struct config *config, int argc, char **argv)
{
    *config = (struct config){
        .model = DEFAULT_MODEL,
        .workers = DEFAULT_WORKERS,
        .timeout = DEFAULT_TIMEOUT,
        .log = false,
        .delay_ms = 0,
        .work_ms = 0,
        .stats_path = NULL,
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
    if (strcmp(name, "--delay") == 0) {
        return number_parse(value, 0, DELAY_MAX, &config->delay_ms);
    }
    if (strcmp(name, "--stats") == 0) {
        config->stats_path = value;
        return true;
    }
    if (strcmp(name, "--work") == 0) {
        return number_parse(value, 0, WORK_MAX, &config->work_ms);
    }

    return false;
}

static bool model_is_known(const char *model)
{
    return strcmp(model, "iter") == 0 || strcmp(model, "fork") == 0 ||
           strcmp(model, "pool") == 0 || strcmp(model, "epoll") == 0 ||
           strcmp(model, "epoll-pool") == 0;
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
    if (strcmp(config->model, "epoll-pool") == 0) {
        return model_epoll_pool_run(listen_fd, config);
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
