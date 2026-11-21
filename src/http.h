#ifndef HTTP_H
#define HTTP_H

#include "net.h"

#include <stdbool.h>

#define HTTP_HEADER_LIMIT 16384
#define HTTP_TARGET_LIMIT 4096
#define HTTP_BODY_LIMIT (1024u * 1024u)
#define HTTP_HEADER_COUNT 64

typedef struct {
    char *name;
    char *value;
} Header;

typedef struct {
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
} Request;

typedef struct {
    HttpHead head;
    int status;
    char *reason;
} UpstreamResponse;

int http_read_request(Stream *stream, Request *request);
int http_read_response(Stream *stream, UpstreamResponse *response);
ssize_t http_read_body(Stream *stream, void *buffer, size_t size);
bool http_forward_header(const HttpHead *head, const char *name);
int http_send_line(Stream *stream, const char *name, const char *value);
int http_send_head(Stream *stream, int status, const char *type, size_t length,
                   const char *allow);
int http_send_error(Stream *stream, int status, const char *allow);

#endif
