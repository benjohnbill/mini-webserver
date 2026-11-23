#include "model.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "response.h"
#include "send_plan.h"
#include "serve.h"

/* epoll_event.data.ptr points to it. The listen socket has NULL. */
struct connection {
    int conn_fd;
    char *buffer; /* heap: grows to HEADER_MAX, then to the request length */
    size_t used;
    size_t capacity;
    bool is_writing; /* plan is valid and the socket waits for EPOLLOUT */
    struct response_plan plan;
    size_t offset; /* bytes of plan already sent */
    time_t last_active;
    struct connection *prev;
    struct connection *next;
};

struct epoll_loop {
    int epoll_fd;
    int listen_fd;
    const struct config *config;
    struct connection *connections; /* list head */
    time_t last_sweep;
};

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

int model_epoll_run(int listen_fd, const struct config *config)
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
    struct epoll_event listen_event = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) < 0) {
        perror("epoll_ctl");
        close(loop.epoll_fd);
        return 1;
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
        int count =
            epoll_wait(loop->epoll_fd, events, EPOLL_EVENTS_MAX, EPOLL_WAIT_MS);
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
    }

    return 0;
}

static int loop_dispatch(struct epoll_loop *loop,
                         const struct epoll_event *event)
{
    struct connection *connection = event->data.ptr;
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

/* Listen socket readable: one new client. Registers it for EPOLLIN. */
static void loop_accept(struct epoll_loop *loop)
{
    int conn_fd = accept(loop->listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        return; /* EINTR, or the client left: the next event tries again */
    }
    int flags = fcntl(conn_fd, F_GETFL);
    struct connection *connection = calloc(1, sizeof(*connection));
    if (flags < 0 || fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        connection == NULL) {
        free(connection);
        close(conn_fd);
        return;
    }
    connection->conn_fd = conn_fd;
    connection->last_active = clock_get_seconds();
    struct epoll_event event = {.events = EPOLLIN, .data.ptr = connection};
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) < 0) {
        free(connection);
        close(conn_fd);
        return;
    }
    connection->next = loop->connections;
    if (loop->connections != NULL) {
        loop->connections->prev = connection;
    }
    loop->connections = connection;
}

/* Full request in the buffer: build the plan, then wait for EPOLLOUT. */
static void connection_reply(struct epoll_loop *loop,
                             struct connection *connection, long frame)
{
    struct request request;
    int status =
        frame < HTTP_INCOMPLETE
            ? (int)-frame
            : http_parse_request(connection->buffer, (size_t)frame, &request);
    if (status == 0) {
        serve_request(&request, loop->config, &connection->plan);
    } else {
        response_plan_init_error(&connection->plan, status, true);
    }
    connection->is_writing = true;
    connection->offset = 0;
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
        if (now - connection->last_active >= loop->config->timeout) {
            connection_close(loop, connection);
        }
        connection = next;
    }
}

static void loop_close(struct epoll_loop *loop)
{
    while (loop->connections != NULL) {
        connection_close(loop, loop->connections);
    }
    close(loop->epoll_fd);
}
