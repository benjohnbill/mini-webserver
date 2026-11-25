#ifndef HTTP_H
#define HTTP_H

#include "net.h"

#include <stdbool.h>

#define HTTP_HEADER_LIMIT 16384
#define HTTP_TARGET_LIMIT 4096
#define HTTP_BODY_LIMIT (1024u * 1024u)
#define HTTP_HEADER_COUNT 64
#define HTTP_MORE (-2)

typedef struct {
    char *name;
    char *value;
} Header;

typedef struct {
    size_t used;
    char raw[HTTP_HEADER_LIMIT + 1];
    char *first_line;
    Header headers[HTTP_HEADER_COUNT];
    size_t count;
    bool has_length;
    size_t length;
    bool has_transfer_encoding;
} HttpHead;

typedef struct {
    HttpHead head;
    char *method;
    char *target;
    char *path;
    char *version;
    unsigned char *body;
    size_t received;
} Request;

typedef struct {
    HttpHead head;
    int status;
    char *reason;
} UpstreamResponse;

typedef struct {
    char data[HTTP_HEADER_LIMIT + 1024];
    size_t length;
} HttpOutput;

/* Advances data/size to the first body byte, without allocating a body. */
int http_feed_request_head(Request *request, const unsigned char **data, size_t *size);
int http_feed_request(Request *request, const unsigned char *data, size_t size);
int http_feed_response(UpstreamResponse *response, const unsigned char **data, size_t *size);
bool http_forward_header(const HttpHead *head, const char *name);
int http_append(HttpOutput *out, const char *format, ...);
int http_make_head(HttpOutput *out, int status, const char *type, size_t length,
                   const char *allow);
int http_make_error(HttpOutput *out, int status, const char *allow);

#endif
