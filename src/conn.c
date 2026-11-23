#include "conn.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "model.h"
#include "send_plan.h"
#include "serve.h"

#define CONN_CLOSED (-2)

struct conn_buffer {
    char *data;
    size_t used;
    size_t capacity;
};

static int connection_read_request(int conn_fd, struct conn_buffer *buffer,
                                   size_t *request_len);
static bool buffer_reserve(struct conn_buffer *buffer, size_t capacity);
static bool connection_read_some(int conn_fd, struct conn_buffer *buffer);
static bool connection_answer(int conn_fd, const struct config *config,
                              struct conn_buffer *buffer, size_t request_len,
                              int status);
static void connection_drain(int conn_fd);
static void delay_sleep(int delay_ms);

void conn_serve(int conn_fd, const struct config *config)
{
    struct conn_buffer buffer = {0};
    struct timeval timeout = {.tv_sec = config->timeout, .tv_usec = 0};
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (!stop_requested) {
        size_t request_len = 0;
        int status = connection_read_request(conn_fd, &buffer, &request_len);
        if (status == CONN_CLOSED ||
            !connection_answer(conn_fd, config, &buffer, request_len, status)) {
            break;
        }
        /* No pipelining: bytes after the body are dropped. */
        buffer.used = 0;
    }
    free(buffer.data);
}

void conn_log(const struct request *request, const struct response_plan *plan)
{
    int status_line_len = (int)strcspn(plan->header, "\r");
    printf("%s %s -> %.*s\n", request != NULL ? request->method : "-",
           request != NULL ? request->target : "-", status_line_len,
           plan->header);
}

/* Returns 0 with *request_len set, a status code, or CONN_CLOSED (the
   client closed, the timeout ran out, or a read error). */
static int connection_read_request(int conn_fd, struct conn_buffer *buffer,
                                   size_t *request_len)
{
    long frame = HTTP_INCOMPLETE;
    while (frame == HTTP_INCOMPLETE || buffer->used < (size_t)frame) {
        size_t want = frame == HTTP_INCOMPLETE ? HEADER_MAX : (size_t)frame;
        if (!buffer_reserve(buffer, want)) {
            return 500;
        }
        if (!connection_read_some(conn_fd, buffer)) {
            return CONN_CLOSED;
        }
        frame = http_frame_measure(buffer->data, buffer->used);
        if (frame < HTTP_INCOMPLETE) {
            return (int)-frame;
        }
    }

    *request_len = (size_t)frame;
    return 0;
}

static bool buffer_reserve(struct conn_buffer *buffer, size_t capacity)
{
    if (buffer->capacity >= capacity) {
        return true;
    }
    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;

    return true;
}

static bool connection_read_some(int conn_fd, struct conn_buffer *buffer)
{
    for (;;) {
        ssize_t length = read(conn_fd, buffer->data + buffer->used,
                              buffer->capacity - buffer->used);
        if (length > 0) {
            buffer->used += (size_t)length;
            return true;
        }
        if (length == 0 || errno != EINTR || stop_requested) {
            return false;
        }
    }
}

/* Returns true if the connection stays open for the next request. */
static bool connection_answer(int conn_fd, const struct config *config,
                              struct conn_buffer *buffer, size_t request_len,
                              int status)
{
    struct request request;
    struct response_plan plan;
    if (config->delay_ms > 0) {
        delay_sleep(config->delay_ms);
    }
    if (status == 0) {
        status = http_parse_request(buffer->data, request_len, &request);
    }
    if (status == 0) {
        serve_request(&request, config, &plan);
    } else {
        response_plan_init_error(&plan, status, true);
    }
    if (config->log) {
        conn_log(status == 0 ? &request : NULL, &plan);
    }

    size_t offset = 0;
    bool keeps_open = send_plan_some(&plan, conn_fd, &offset) == SEND_DONE &&
                      !plan.close_after;
    response_plan_close(&plan);
    if (status != 0) {
        connection_drain(conn_fd);
    }

    return keeps_open;
}

/* After an error response, unread request bytes make close() send RST, and
   the client can lose the response. Stop writing, then read and drop the
   rest for a short time, so that close() sends FIN. */
static void connection_drain(int conn_fd)
{
    char scrap[DRAIN_CHUNK_SIZE];
    struct pollfd watch = {.fd = conn_fd, .events = POLLIN};
    shutdown(conn_fd, SHUT_WR);
    for (int i = 0; i < DRAIN_READS_MAX; i++) {
        if (poll(&watch, 1, DRAIN_WAIT_MS) <= 0 ||
            read(conn_fd, scrap, sizeof(scrap)) <= 0) {
            return;
        }
    }
}

/* --delay: a slow origin. A signal ends the sleep early. */
static void delay_sleep(int delay_ms)
{
    struct timespec delay = {.tv_sec = delay_ms / 1000,
                             .tv_nsec = (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
