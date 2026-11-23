#include "response.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ERROR_TEXT_SIZE 64
#define ALLOW_LINE_SIZE 64

struct plan_head {
    int status;
    const char *content_type;
    off_t content_length;
    bool close_after;
    const char *allowed_method; /* NULL: no Allow header */
    const char *text;           /* NULL: the body is not in header[] */
};

static bool plan_write_head(struct response_plan *plan,
                            const struct plan_head *head);
static const char *status_get_reason(int status);
static void plan_write_error(struct response_plan *plan, int status,
                             bool close_after, const char *allowed_method);

bool response_plan_init(struct response_plan *plan, int status,
                        const char *content_type, off_t content_length,
                        bool close_after)
{
    struct plan_head head = {
        .status = status,
        .content_type = content_type,
        .content_length = content_length,
        .close_after = close_after,
    };

    return plan_write_head(plan, &head);
}

void response_plan_init_text(struct response_plan *plan, int status,
                             const char *content_type, const char *text,
                             bool close_after)
{
    struct plan_head head = {
        .status = status,
        .content_type = content_type,
        .content_length = (off_t)strlen(text),
        .close_after = close_after,
        .text = text,
    };
    plan_write_head(plan, &head);
}

void response_plan_init_error(struct response_plan *plan, int status,
                              bool close_after)
{
    plan_write_error(plan, status, close_after,
                     status == 405 ? "GET, POST" : NULL);
}

void response_plan_init_not_allowed(struct response_plan *plan,
                                    const char *allowed_method)
{
    plan_write_error(plan, 405, true, allowed_method);
}

void response_plan_close(struct response_plan *plan)
{
    if (plan->file_fd >= 0) {
        close(plan->file_fd);
        plan->file_fd = -1;
    }
}

static bool plan_write_head(struct response_plan *plan,
                            const struct plan_head *head)
{
    char allow_line[ALLOW_LINE_SIZE] = "";
    if (head->allowed_method != NULL) {
        snprintf(allow_line, sizeof(allow_line), "Allow: %s\r\n",
                 head->allowed_method);
    }
    int length = snprintf(plan->header, sizeof(plan->header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %lld\r\n"
                          "Connection: %s\r\n"
                          "%s\r\n%s",
                          head->status, status_get_reason(head->status),
                          head->content_type, (long long)head->content_length,
                          head->close_after ? "close" : "keep-alive",
                          allow_line, head->text != NULL ? head->text : "");

    plan->body = NULL;
    plan->body_len = 0;
    plan->file_fd = -1;
    plan->file_size = 0;
    plan->close_after = head->close_after;
    /* A long Content-Type from POST /echo can overflow header[]. Send a 500
       instead of a cut header. */
    if (length < 0 || (size_t)length >= sizeof(plan->header)) {
        plan_write_error(plan, 500, true, NULL);
        return false;
    }
    plan->header_len = (size_t)length;

    return true;
}

static const char *status_get_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

static void plan_write_error(struct response_plan *plan, int status,
                             bool close_after, const char *allowed_method)
{
    char text[ERROR_TEXT_SIZE];
    snprintf(text, sizeof(text), "%d %s\n", status, status_get_reason(status));
    struct plan_head head = {
        .status = status,
        .content_type = "text/plain",
        .content_length = (off_t)strlen(text),
        .close_after = close_after,
        .allowed_method = allowed_method,
        .text = text,
    };
    plan_write_head(plan, &head);
}
