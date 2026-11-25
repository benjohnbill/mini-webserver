#include "connection.h"
#include "event_loop.h"
#include "proxy_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WORKER_COUNT = 4, QUEUE_CAPACITY = 64, CHILD_LIMIT = 64 };

typedef struct {
    const ServerConfig *config;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int sockets[QUEUE_CAPACITY];
    size_t head;
    size_t count;
    bool stopping;
} WorkerPool;

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int setup_signals(void)
{
    struct sigaction stop = {.sa_handler = request_stop};
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&stop.sa_mask);
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGINT, &stop, NULL) == -1 ||
        sigaction(SIGTERM, &stop, NULL) == -1 ||
        sigaction(SIGPIPE, &ignore, NULL) == -1)
        return -1;
    return 0;
}

static void *serve_connections(void *argument)
{
    WorkerPool *pool = argument;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->count == 0 && !pool->stopping)
            pthread_cond_wait(&pool->ready, &pool->mutex);
        if (pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        int fd = pool->sockets[pool->head];
        pool->head = (pool->head + 1) % QUEUE_CAPACITY;
        pool->count--;
        pthread_mutex_unlock(&pool->mutex);

        handle_client(fd, pool->config);
        close(fd);
    }
}

static bool enqueue(WorkerPool *pool, int fd)
{
    pthread_mutex_lock(&pool->mutex);
    bool queued = pool->count < QUEUE_CAPACITY;
    if (queued) {
        size_t tail = (pool->head + pool->count) % QUEUE_CAPACITY;
        pool->sockets[tail] = fd;
        pool->count++;
        pthread_cond_signal(&pool->ready);
    }
    pthread_mutex_unlock(&pool->mutex);
    return queued;
}

static void stop_workers(WorkerPool *pool, pthread_t *threads, size_t count)
{
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    for (size_t i = 0; i < pool->count; i++)
        close(pool->sockets[(pool->head + i) % QUEUE_CAPACITY]);
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < count; i++)
        pthread_join(threads[i], NULL);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->mutex);
}

enum { ACCEPT_AGAIN = -2 };

static int next_client(int listener)
{
    struct pollfd incoming = {.fd = listener, .events = POLLIN};
    int ready = poll(&incoming, 1, 200);
    if (ready == 0 || (ready < 0 && errno == EINTR) || stop_requested)
        return ACCEPT_AGAIN;
    if (ready < 0)
        return -1;
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
            return ACCEPT_AGAIN;
        return -1;
    }
    if (net_nonblocking(fd) < 0) {
        close(fd);
        return ACCEPT_AGAIN;
    }
    return fd;
}

static int accept_connections(int listener, WorkerPool *pool)
{
    while (!stop_requested) {
        int fd = next_client(listener);
        if (fd == ACCEPT_AGAIN)
            continue;
        if (fd < 0)
            return -1;
        if (!enqueue(pool, fd))
            close(fd);
    }
    return 0;
}

static int run_fork(int listener, const ServerConfig *config)
{
    int children = 0;
    int result = 0;
    while (!stop_requested) {
        while (waitpid(-1, NULL, WNOHANG) > 0)
            children--;
        int fd = next_client(listener);
        if (fd == ACCEPT_AGAIN)
            continue;
        if (fd < 0) {
            result = -1;
            break;
        }
        if (children < CHILD_LIMIT) {
            pid_t pid = fork();
            if (pid == 0) {
                close(listener);
                handle_client(fd, config);
                close(fd);
                close(config->root_fd);
                _exit(0);
            }
            if (pid > 0)
                children++;
            else
                perror("fork");
        }
        close(fd);
    }
    while (children > 0) {
        if (waitpid(-1, NULL, 0) > 0)
            children--;
        else if (errno != EINTR)
            break;
    }
    return result;
}

static int run_pool(int listener, const ServerConfig *config)
{
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &shutdown_signals, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        return -1;
    }

    /* Workers inherit the mask. Only the main thread handles shutdown signals. */
    WorkerPool pool = {
        .config = config,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .ready = PTHREAD_COND_INITIALIZER
    };
    pthread_t threads[WORKER_COUNT];
    size_t started = 0;
    for (; started < WORKER_COUNT; started++) {
        error = pthread_create(&threads[started], NULL, serve_connections, &pool);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            break;
        }
    }

    int result = -1;
    error = pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
    } else if (started == WORKER_COUNT) {
        result = accept_connections(listener, &pool);
    }

    stop_workers(&pool, threads, started);
    return result;
}

static int serve(ServerConfig *config, const char *root, const char *bind_address, uint16_t port,
                 const char *model, enum EventModel event_model, bool pool, bool forking)
{
    config->root_fd = open(root, O_RDONLY | O_DIRECTORY);
    if (config->root_fd == -1) {
        perror("open document root");
        return 1;
    }

    int listener = -1;
    if (setup_signals() == -1) {
        perror("sigaction");
    } else {
        listener = net_listen(bind_address, port);
        if (listener == -1)
            perror("listen");
    }
    if (listener == -1) {
        close(config->root_fd);
        return 1;
    }

    if (config->cache_enabled || config->rate) {
        config->policy = proxy_policy_new();
        if (!config->policy) {
            fprintf(stderr, "Failed to initialize shared proxy policy\n");
            close(listener);
            close(config->root_fd);
            return 1;
        }
    }
    printf("Listening on http://%s:%u (%s)\n", bind_address, (unsigned)port, model);
    fflush(stdout);
    int result;
    if (pool)
        result = run_pool(listener, config);
    else if (forking)
        result = run_fork(listener, config);
    else
        result = run_event_loop(listener, config, event_model, &stop_requested);
    if (result < 0)
        perror("server");
    close(listener);
    close(config->root_fd);
    proxy_policy_free(config->policy);
    return result < 0 ? 1 : 0;
}

int main(int argc, char *argv[])
{
    const char *bind_address = "127.0.0.1";
    const char *model = "pthread";
    ServerConfig config = {0};
    int option;
    while ((option = getopt(argc, argv, "b:m:cr:B:")) != -1) {
        if (option == 'b')
            bind_address = optarg;
        else if (option == 'm')
            model = optarg;
        else if (option == 'c')
            config.cache_enabled = true;
        else if (option == 'r' && parse_port(optarg, &config.rate))
            continue;
        else if (option == 'B' && parse_port(optarg, &config.burst))
            continue;
        else {
            fprintf(stderr, "Invalid option or rate/burst value\n");
            return 2;
        }
    }

    enum EventModel event_model = USE_POLL;
    bool pool = strcmp(model, "pthread") == 0;
    bool forking = strcmp(model, "fork") == 0;
    if (strcmp(model, "select") == 0)
        event_model = USE_SELECT;
#ifdef NATIVE_EVENT_MODEL
    else if (strcmp(model, NATIVE_EVENT_MODEL) == 0)
        event_model = USE_NATIVE;
#endif
    else if (!pool && !forking && strcmp(model, "poll") != 0) {
        fprintf(stderr, "Unsupported concurrency model: %s (available: pthread fork select poll"
#ifdef NATIVE_EVENT_MODEL
                " " NATIVE_EVENT_MODEL
#endif
                ")\n", model);
        return 2;
    }

    uint16_t port;
    if (argc - optind < 2 || !parse_port(argv[optind], &port) ||
        argc - optind - 2 > ROUTE_LIMIT || (config.burst && !config.rate)) {
        fprintf(stderr, "Usage: %s [-b listen-ip] [-m model] [-c] [-r requests/sec [-B burst]] PORT ROOT [/path=IP:PORT[,IP:PORT...] ...]\n", argv[0]);
        return 2;
    }

    if (!config.burst)
        config.burst = config.rate;
    int result = 2;
    for (int i = optind + 2; i < argc; i++) {
        Route *route = &config.routes[config.route_count];
        if (!parse_route(argv[i], route)) {
            fprintf(stderr, "Invalid proxy route: %s\n", argv[i]);
            goto done;
        }
        for (size_t j = 0; j < config.route_count; j++) {
            if (strcmp(config.routes[j].prefix, route->prefix) == 0) {
                fprintf(stderr, "Duplicate proxy route: %s\n", route->prefix);
                free(route->ring);
                goto done;
            }
        }
        config.route_count++;
    }

    result = serve(&config, argv[optind + 1], bind_address, port, model, event_model, pool, forking);
done:
    for (size_t i = 0; i < config.route_count; i++)
        free(config.routes[i].ring);
    return result;
}
