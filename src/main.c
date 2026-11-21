#include "handlers.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { WORKER_COUNT = 4, QUEUE_CAPACITY = 64 };

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

static int accept_connections(int listener, WorkerPool *pool)
{
    struct pollfd incoming = {.fd = listener, .events = POLLIN};
    while (!stop_requested) {
        int ready = poll(&incoming, 1, 200);
        if (ready == -1 && errno != EINTR) {
            perror("poll");
            return -1;
        }
        if (ready <= 0 || stop_requested)
            continue;
        if ((incoming.revents & POLLIN) == 0) {
            fprintf(stderr, "listening socket became unavailable\n");
            return -1;
        }

        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED)
                continue;
            perror("accept");
            return -1;
        }
        if (net_nonblocking(fd) == -1 || !enqueue(pool, fd))
            close(fd);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *bind_address = "127.0.0.1";
    int option;
    while ((option = getopt(argc, argv, "b:")) != -1) {
        if (option != 'b')
            return 2;
        bind_address = optarg;
    }

    uint16_t port;
    if (argc - optind < 2 || !parse_port(argv[optind], &port) ||
        argc - optind - 2 > ROUTE_LIMIT) {
        fprintf(stderr, "Usage: %s [-b listen-ip] PORT ROOT [/path=IP:PORT ...]\n", argv[0]);
        return 2;
    }

    ServerConfig config = {0};
    for (int i = optind + 2; i < argc; i++) {
        Route *route = &config.routes[config.route_count];
        if (!parse_route(argv[i], route)) {
            fprintf(stderr, "Invalid proxy route: %s\n", argv[i]);
            return 2;
        }
        for (size_t j = 0; j < config.route_count; j++) {
            if (strcmp(config.routes[j].prefix, route->prefix) == 0) {
                fprintf(stderr, "Duplicate proxy route: %s\n", route->prefix);
                return 2;
            }
        }
        config.route_count++;
    }

    config.root_fd = open(argv[optind + 1], O_RDONLY | O_DIRECTORY);
    if (config.root_fd == -1) {
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
        close(config.root_fd);
        return 1;
    }

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &shutdown_signals, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        close(listener);
        close(config.root_fd);
        return 1;
    }

    /* Workers inherit the mask. Only the main thread handles shutdown signals. */
    WorkerPool pool = {
        .config = &config,
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

    int result = 1;
    error = pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
    } else if (started == WORKER_COUNT) {
        printf("Listening on http://%s:%u with %d workers\n",
               bind_address, (unsigned)port, WORKER_COUNT);
        fflush(stdout);
        result = accept_connections(listener, &pool) == 0 ? 0 : 1;
    }

    close(listener);
    stop_workers(&pool, threads, started);
    close(config.root_fd);
    return result;
}
