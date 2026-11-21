#ifndef RESPONSE_H
#define RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define RESPONSE_HEADER_MAX 1024

struct response_plan {
    char header[RESPONSE_HEADER_MAX];
    size_t header_len;
    const char *body; /* memory body, or NULL */
    size_t body_len;
    int file_fd; /* -1 if no file */
    off_t file_size;
    bool close_after;
};

/* The caller sets body or file_fd after this call, if it returns true.
   False: the header did not fit, and the plan is a 500 now. */
bool response_plan_init(struct response_plan *plan, int status,
                        const char *content_type, off_t content_length,
                        bool close_after);
/* A short generated body goes into header[], so the plan owns it. */
void response_plan_init_text(struct response_plan *plan, int status,
                             const char *content_type, const char *text,
                             bool close_after);
void response_plan_init_error(struct response_plan *plan, int status,
                              bool close_after);
/* 405 with "Allow: <allowed_method>". It closes the connection. */
void response_plan_init_not_allowed(struct response_plan *plan,
                                    const char *allowed_method);
void response_plan_close(struct response_plan *plan); /* closes file_fd */

#endif
