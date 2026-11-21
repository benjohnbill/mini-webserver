#include "send_plan.h"

#include <errno.h>
#include <unistd.h>

#include "config.h"

static size_t plan_body_len(const struct response_plan *plan);
static ssize_t plan_send_part(const struct response_plan *plan, int conn_fd,
                              size_t offset);
static ssize_t plan_send_file_chunk(const struct response_plan *plan,
                                    int conn_fd, size_t body_offset);

int send_plan_some(const struct response_plan *plan, int conn_fd,
                   size_t *offset)
{
    size_t total = plan->header_len + plan_body_len(plan);
    while (*offset < total) {
        ssize_t written = plan_send_part(plan, conn_fd, *offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK ? SEND_AGAIN
                                                           : SEND_ERROR;
        }
        *offset += (size_t)written;
    }
    return SEND_DONE;
}

static size_t plan_body_len(const struct response_plan *plan)
{
    return plan->file_fd >= 0 ? (size_t)plan->file_size : plan->body_len;
}

static ssize_t plan_send_part(const struct response_plan *plan, int conn_fd,
                              size_t offset)
{
    if (offset < plan->header_len) {
        return write(conn_fd, plan->header + offset, plan->header_len - offset);
    }
    size_t body_offset = offset - plan->header_len;
    if (plan->file_fd < 0) {
        return write(conn_fd, plan->body + body_offset,
                     plan->body_len - body_offset);
    }
    return plan_send_file_chunk(plan, conn_fd, body_offset);
}

/* pread, not sendfile: sendfile is not POSIX. */
static ssize_t plan_send_file_chunk(const struct response_plan *plan,
                                    int conn_fd, size_t body_offset)
{
    char chunk[SEND_CHUNK_SIZE];
    size_t remaining = (size_t)plan->file_size - body_offset;
    size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    ssize_t length = pread(plan->file_fd, chunk, want, (off_t)body_offset);
    if (length == 0) {
        errno = EIO; /* the file became shorter after stat */
        return -1;
    }
    if (length < 0) {
        return -1;
    }
    return write(conn_fd, chunk, (size_t)length);
}
