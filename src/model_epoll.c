#include "model.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "conn.h"
#include "response.h"
#include "send_plan.h"
#include "serve.h"

/* epoll_event.data.ptr points to it. The listen socket has NULL, and the
   wake fd of the Workers has &wake_mark. */
struct connection {
    int conn_fd;
    char *buffer; /* heap: grows to HEADER_MAX, then to the request length */
    size_t used;
    size_t capacity;
    bool is_writing; /* plan is valid and the socket waits for EPOLLOUT */
    /* epoll-pool: a Worker makes the plan. Only the Worker touches buffer
       and plan until the loop takes the connection back. */
    bool is_working;
    long frame; /* epoll-pool: the request frame for the Worker */
    struct response_plan plan;
    size_t offset; /* bytes of plan already sent */
    time_t last_active;
    struct connection *prev;
    struct connection *next;
    struct connection *job_next; /* epoll-pool: in the job or done list */
};

/* epoll-pool: the loop gives a full request to a Worker, and the Worker
   gives back the connection with its plan. A slow client then waits in
   the loop, not in a Worker. */
struct work_pool {
    pthread_mutex_t lock;
    pthread_cond_t has_job;
    struct connection *jobs; /* oldest first */
    struct connection *jobs_tail;
    struct connection *done; /* plans that wait for the loop */
    bool is_closed;
    int wake_fd; /* eventfd: a Worker writes 1 when it adds to done */
    const struct config *config;
    pthread_t *threads;
    int thread_count;
    atomic_int busy; /* Workers that make a plan now, for --stats */
};

struct epoll_loop {
    int epoll_fd;
    int listen_fd;
    const struct config *config;
    struct connection *connections; /* list head */
    int connection_count;
    time_t last_sweep;
    double stats_due;       /* --stats: the next write, monotonic seconds */
    struct work_pool *pool; /* NULL in the epoll model */
};

#define ACCEPT_BURST 64

static struct connection wake_mark;

static int epoll_serve(int listen_fd, const struct config *config,
                       bool has_pool);
static time_t clock_get_seconds(void);
static int loop_run(struct epoll_loop *loop);
static int loop_dispatch(struct epoll_loop *loop,
                         const struct epoll_event *event);
static void connection_send(struct epoll_loop *loop,
                            struct connection *connection);
static int connection_watch(struct epoll_loop *loop,
                            struct connection *connection, uint32_t events);
static void connection_close(struct epoll_loop *loop,
                             struct connection *connection);
static void connection_unlink(struct epoll_loop *loop,
                              struct connection *connection);
static int loop_handle_input(struct epoll_loop *loop,
                             const struct epoll_event *event);
static void loop_sweep_timeouts(struct epoll_loop *loop);
static void loop_close(struct epoll_loop *loop);
static bool loop_accept_one(struct epoll_loop *loop);
static void plan_build(struct connection *connection,
                       const struct config *config);
static void connection_start_send(struct epoll_loop *loop,
                                  struct connection *connection);
static struct work_pool *pool_create(int epoll_fd, const struct config *config);
static void pool_start_workers(struct work_pool *pool);
static void pool_stop(struct work_pool *pool);
static void pool_push(struct work_pool *pool, struct connection *connection);
static void *worker_main(void *arg);
static void worker_give_back(struct work_pool *pool,
                             struct connection *connection);
static void loop_take_done(struct epoll_loop *loop);
static int loop_get_wait_ms(const struct epoll_loop *loop);
static void loop_publish_stats(struct epoll_loop *loop);

int model_epoll_run(int listen_fd, const struct config *config)
{
    return epoll_serve(listen_fd, config, false);
}

int model_epoll_pool_run(int listen_fd, const struct config *config)
{
    return epoll_serve(listen_fd, config, true);
}

static int epoll_serve(int listen_fd, const struct config *config,
                       bool has_pool)
{
    struct epoll_loop loop = {
        .listen_fd = listen_fd,
        .config = config,
        .connections = NULL,
        .last_sweep = clock_get_seconds(),
    };
    loop.epoll_fd = epoll_create1(0);
    if (loop.epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }
    int listen_flags = fcntl(listen_fd, F_GETFL);
    if (listen_flags < 0 ||
        fcntl(listen_fd, F_SETFL, listen_flags | O_NONBLOCK) < 0) {
        perror("fcntl");
        return 1;
    }
    struct epoll_event listen_event = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) < 0) {
        perror("epoll_ctl");
        close(loop.epoll_fd);
        return 1;
    }
    if (has_pool) {
        loop.pool = pool_create(loop.epoll_fd, config);
        if (loop.pool == NULL) {
            close(loop.epoll_fd);
            return 1;
        }
    }

    int result = loop_run(&loop);
    loop_close(&loop);
    return result;
}

static time_t clock_get_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

static int loop_run(struct epoll_loop *loop)
{
    struct epoll_event events[EPOLL_EVENTS_MAX];
    while (!stop_requested) {
        int count = epoll_wait(loop->epoll_fd, events, EPOLL_EVENTS_MAX,
                               loop_get_wait_ms(loop));
        if (count < 0 && errno != EINTR) {
            perror("epoll_wait");
            return 1;
        }
        for (int i = 0; i < count; i++) {
            /* -1 while the read side is a slot, 1 on an error */
            int result = loop_dispatch(loop, &events[i]);
            if (result != 0) {
                return result;
            }
        }
        loop_sweep_timeouts(loop);
        loop_publish_stats(loop);
    }

    return 0;
}

/* With --stats the loop wakes often enough to keep the file fresh. */
static int loop_get_wait_ms(const struct epoll_loop *loop)
{
    return loop->config->stats_path != NULL ? STATS_PERIOD_MS : EPOLL_WAIT_MS;
}

/* --stats: one line for the demo dashboard, "connections C workers W busy
   B". A .tmp file and a rename, so a reader never sees half a file. */
static void loop_publish_stats(struct epoll_loop *loop)
{
    const char *path = loop->config->stats_path;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double seconds = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
    if (path == NULL || seconds < loop->stats_due) {
        return;
    }
    loop->stats_due = seconds + (double)STATS_PERIOD_MS / 1000.0;
    char tmp_path[PATH_BUFFER_SIZE];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *file = fopen(tmp_path, "w");
    if (file == NULL) {
        return; /* the next try comes STATS_PERIOD_MS later */
    }
    int workers = loop->pool != NULL ? loop->pool->thread_count : 0;
    int busy = loop->pool != NULL ? atomic_load(&loop->pool->busy) : 0;
    bool is_written = fprintf(file, "connections %d workers %d busy %d\n",
                              loop->connection_count, workers, busy) > 0;
    if (fclose(file) == 0 && is_written) {
        rename(tmp_path, path);
    }
}

static int loop_dispatch(struct epoll_loop *loop,
                         const struct epoll_event *event)
{
    struct connection *connection = event->data.ptr;
    if (connection == &wake_mark) {
        loop_take_done(loop);
        return 0;
    }
    if (connection != NULL && connection->is_working) {
        return 0; /* a hang-up waits until the Worker gives it back */
    }
    if (connection != NULL && connection->is_writing) {
        connection_send(loop, connection);
        return 0;
    }

    return loop_handle_input(loop, event);
}

static void connection_send(struct epoll_loop *loop,
                            struct connection *connection)
{
    int result = send_plan_some(&connection->plan, connection->conn_fd,
                                &connection->offset);
    connection->last_active = clock_get_seconds();
    if (result == SEND_AGAIN) {
        return;
    }

    bool keeps_open = result == SEND_DONE && !connection->plan.close_after;
    response_plan_close(&connection->plan);
    connection->is_writing = false;
    if (!keeps_open || connection_watch(loop, connection, EPOLLIN) != 0) {
        connection_close(loop, connection);
        return;
    }
    /* No pipelining: bytes after the body are dropped. */
    connection->used = 0;
    connection->offset = 0;
}

static int connection_watch(struct epoll_loop *loop,
                            struct connection *connection, uint32_t events)
{
    struct epoll_event event = {.events = events, .data.ptr = connection};
    return epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, connection->conn_fd,
                     &event);
}

static void connection_close(struct epoll_loop *loop,
                             struct connection *connection)
{
    if (connection->is_writing) {
        response_plan_close(&connection->plan);
    }
    close(connection->conn_fd);
    connection_unlink(loop, connection);
    loop->connection_count--;
    free(connection->buffer);
    free(connection);
}

static void connection_unlink(struct epoll_loop *loop,
                              struct connection *connection)
{
    if (connection->prev != NULL) {
        connection->prev->next = connection->next;
    } else {
        loop->connections = connection->next;
    }
    if (connection->next != NULL) {
        connection->next->prev = connection->prev;
    }
}

/* Listen socket readable: up to ACCEPT_BURST new clients. One accept for
   each turn was too slow: with thousands of open sends in the same turn,
   the backlog grew and rproxy gave 504 to 4427 of 10000 Phones (demo Act
   4, 2026-11-26). */
static void loop_accept(struct epoll_loop *loop)
{
    for (int i = 0; i < ACCEPT_BURST; i++) {
        if (!loop_accept_one(loop)) {
            return;
        }
    }
}

/* Returns false when the backlog is empty (the listen socket does not
   block) or on an error. Registers the client for EPOLLIN. */
static bool loop_accept_one(struct epoll_loop *loop)
{
    int conn_fd = accept(loop->listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        return false; /* EAGAIN, EINTR, or the client left */
    }
    int flags = fcntl(conn_fd, F_GETFL);
    struct connection *connection = calloc(1, sizeof(*connection));
    if (flags < 0 || fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        connection == NULL) {
        free(connection);
        close(conn_fd);
        return true;
    }
    connection->conn_fd = conn_fd;
    connection->last_active = clock_get_seconds();
    conn_send_buffer_cap(conn_fd, loop->config);
    struct epoll_event event = {.events = EPOLLIN, .data.ptr = connection};
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) < 0) {
        free(connection);
        close(conn_fd);
        return true;
    }
    connection->next = loop->connections;
    if (loop->connections != NULL) {
        loop->connections->prev = connection;
    }
    loop->connections = connection;
    loop->connection_count++;

    return true;
}

/* Full request in the buffer: build the plan, then wait for EPOLLOUT. In
   epoll-pool a Worker builds the plan, and the loop serves the others. */
static void connection_reply(struct epoll_loop *loop,
                             struct connection *connection, long frame)
{
    connection->frame = frame;
    if (loop->pool != NULL) {
        /* No events while a Worker has it. EPOLLHUP still comes, and
           loop_dispatch drops it. */
        connection->is_working = true;
        if (connection_watch(loop, connection, 0) != 0) {
            connection_close(loop, connection);
            return;
        }
        pool_push(loop->pool, connection);
        return;
    }
    plan_build(connection, loop->config);
    connection_start_send(loop, connection);
}

/* The Shared core part of one request. --work burns CPU first: the
   Handler time of a real Origin. */
static void plan_build(struct connection *connection,
                       const struct config *config)
{
    conn_work_burn(config->work_ms);
    long frame = connection->frame;
    struct request request;
    int status =
        frame < HTTP_INCOMPLETE
            ? (int)-frame
            : http_parse_request(connection->buffer, (size_t)frame, &request);
    if (status == 0) {
        serve_request(&request, config, &connection->plan);
    } else {
        response_plan_init_error(&connection->plan, status, true);
    }
}

static void connection_start_send(struct epoll_loop *loop,
                                  struct connection *connection)
{
    connection->is_writing = true;
    connection->offset = 0;
    connection->last_active = clock_get_seconds();
    if (connection_watch(loop, connection, EPOLLOUT) != 0) {
        connection_close(loop, connection);
    }
}

/* Client socket readable: read what has arrived, never wait for more. */
static void connection_receive(struct epoll_loop *loop,
                               struct connection *connection)
{
    long frame = connection->used == 0
                     ? HTTP_INCOMPLETE
                     : http_frame_measure(connection->buffer, connection->used);
    size_t want = frame > 0 ? (size_t)frame : HEADER_MAX;
    if (connection->capacity < want) {
        char *buffer = realloc(connection->buffer, want);
        if (buffer == NULL) {
            connection_close(loop, connection);
            return;
        }
        connection->buffer = buffer;
        connection->capacity = want;
    }

    ssize_t length =
        read(connection->conn_fd, connection->buffer + connection->used,
             connection->capacity - connection->used);
    if (length < 0 && (errno == EAGAIN || errno == EINTR)) {
        return; /* nothing yet: back to epoll_wait, serve the others */
    }
    if (length <= 0) {
        connection_close(loop, connection);
        return;
    }
    connection->used += (size_t)length;
    connection->last_active = clock_get_seconds();

    frame = http_frame_measure(connection->buffer, connection->used);
    if (frame == HTTP_INCOMPLETE ||
        (frame > 0 && connection->used < (size_t)frame)) {
        return; /* request not complete: remember it, serve the others */
    }
    connection_reply(loop, connection, frame);
}

static int loop_handle_input(struct epoll_loop *loop,
                             const struct epoll_event *event)
{
    struct connection *connection = event->data.ptr;
    if (connection == NULL) {
        loop_accept(loop);
    } else {
        connection_receive(loop, connection);
    }
    return 0;
}

/* A connection at a Worker is not idle: the sweep skips it. */
static void loop_sweep_timeouts(struct epoll_loop *loop)
{
    time_t now = clock_get_seconds();
    if (now - loop->last_sweep < 1) {
        return;
    }
    loop->last_sweep = now;

    struct connection *connection = loop->connections;
    while (connection != NULL) {
        struct connection *next = connection->next;
        if (!connection->is_working &&
            now - connection->last_active >= loop->config->timeout) {
            connection_close(loop, connection);
        }
        connection = next;
    }
}

/* The Workers stop first, so no Worker touches a connection that the loop
   frees. */
static void loop_close(struct epoll_loop *loop)
{
    if (loop->pool != NULL) {
        pool_stop(loop->pool);
        loop_take_done(loop);
        pthread_mutex_destroy(&loop->pool->lock);
        pthread_cond_destroy(&loop->pool->has_job);
        close(loop->pool->wake_fd);
        free(loop->pool);
    }
    while (loop->connections != NULL) {
        connection_close(loop, loop->connections);
    }
    close(loop->epoll_fd);
}

static struct work_pool *pool_create(int epoll_fd, const struct config *config)
{
    struct work_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->config = config;
    pool->wake_fd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event event = {.events = EPOLLIN, .data.ptr = &wake_mark};
    pool->threads = calloc((size_t)config->workers, sizeof(*pool->threads));
    if (pool->wake_fd < 0 || pool->threads == NULL ||
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pool->wake_fd, &event) < 0) {
        perror("epoll-pool");
        if (pool->wake_fd >= 0) {
            close(pool->wake_fd);
        }
        free(pool->threads);
        free(pool);
        return NULL;
    }
    atomic_init(&pool->busy, 0);
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->has_job, NULL);
    pool_start_workers(pool);

    return pool;
}

/* Workers inherit this mask, so only the main thread gets the signals. */
static void pool_start_workers(struct work_pool *pool)
{
    sigset_t blocked;
    sigset_t old;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked, &old);
    for (int i = 0; i < pool->config->workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) == 0) {
            pool->thread_count++;
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* A Worker ends its current job, then stops. Jobs still in the list stay
   in loop->connections, and loop_close frees them. */
static void pool_stop(struct work_pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    pool->is_closed = true;
    pthread_cond_broadcast(&pool->has_job);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
}

static void pool_push(struct work_pool *pool, struct connection *connection)
{
    connection->job_next = NULL;
    pthread_mutex_lock(&pool->lock);
    if (pool->jobs_tail != NULL) {
        pool->jobs_tail->job_next = connection;
    } else {
        pool->jobs = connection;
    }
    pool->jobs_tail = connection;
    pthread_cond_signal(&pool->has_job);
    pthread_mutex_unlock(&pool->lock);
}

static void *worker_main(void *arg)
{
    struct work_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->jobs == NULL && !pool->is_closed) {
            pthread_cond_wait(&pool->has_job, &pool->lock);
        }
        struct connection *connection = pool->jobs;
        if (connection == NULL) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        pool->jobs = connection->job_next;
        if (pool->jobs == NULL) {
            pool->jobs_tail = NULL;
        }
        pthread_mutex_unlock(&pool->lock);

        atomic_fetch_add(&pool->busy, 1);
        plan_build(connection, pool->config);
        atomic_fetch_sub(&pool->busy, 1);
        worker_give_back(pool, connection);
    }
}

static void worker_give_back(struct work_pool *pool,
                             struct connection *connection)
{
    pthread_mutex_lock(&pool->lock);
    connection->job_next = pool->done;
    pool->done = connection;
    pthread_mutex_unlock(&pool->lock);
    uint64_t one = 1;
    if (write(pool->wake_fd, &one, sizeof(one)) < 0) {
        /* EAGAIN: the counter is full, and the loop wakes anyway. */
    }
}

/* The loop takes back the connections that have a plan, and sends. */
static void loop_take_done(struct epoll_loop *loop)
{
    uint64_t count;
    if (read(loop->pool->wake_fd, &count, sizeof(count)) < 0) {
        /* EAGAIN: no write since the last read, the list can still have
           connections from before. */
    }
    pthread_mutex_lock(&loop->pool->lock);
    struct connection *connection = loop->pool->done;
    loop->pool->done = NULL;
    pthread_mutex_unlock(&loop->pool->lock);
    while (connection != NULL) {
        struct connection *next = connection->job_next;
        connection->is_working = false;
        connection_start_send(loop, connection);
        connection = next;
    }
}
