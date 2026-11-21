#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "model.h"

#define QUEUE_SIZE 64

struct pool {
    int fds[QUEUE_SIZE];
    int head;
    int count;
    bool closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    const struct config *config;
};

static void pool_push(struct pool *pool, int conn_fd)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == QUEUE_SIZE) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }
    pool->fds[(pool->head + pool->count) % QUEUE_SIZE] = conn_fd;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
}

/* Returns -1 when the pool is closed and the queue is empty. */
static int pool_pop(struct pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == 0 && !pool->closed) {
        pthread_cond_wait(&pool->not_empty, &pool->lock);
    }
    int conn_fd = -1;
    if (pool->count > 0) {
        conn_fd = pool->fds[pool->head];
        pool->head = (pool->head + 1) % QUEUE_SIZE;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
    }
    pthread_mutex_unlock(&pool->lock);
    return conn_fd;
}

static void *worker_main(void *arg)
{
    struct pool *pool = arg;
    for (;;) {
        int conn_fd = pool_pop(pool);
        if (conn_fd < 0) {
            return NULL;
        }
        conn_serve(conn_fd, pool->config);
        close(conn_fd);
    }
}

int model_pool_run(int listen_fd, const struct config *config)
{
    struct pool pool = {.config = config};
    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.not_empty, NULL);
    pthread_cond_init(&pool.not_full, NULL);

    /* Workers inherit this mask, so only the main thread gets the signals. */
    sigset_t blocked;
    sigset_t old;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &blocked, &old);

    pthread_t *workers = calloc((size_t)config->workers, sizeof(*workers));
    if (workers == NULL) {
        return -1;
    }
    for (int i = 0; i < config->workers; i++) {
        pthread_create(&workers[i], NULL, worker_main, &pool);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    int result = 0;
    while (!stop_requested) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = -1;
            break;
        }
        pool_push(&pool, conn_fd);
    }

    pthread_mutex_lock(&pool.lock);
    pool.closed = true;
    pthread_cond_broadcast(&pool.not_empty);
    pthread_mutex_unlock(&pool.lock);
    for (int i = 0; i < config->workers; i++) {
        pthread_join(workers[i], NULL);
    }
    free(workers);
    return result;
}
