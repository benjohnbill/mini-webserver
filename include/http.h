#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

/* Limits. Requests exceeding them are rejected with the noted status. */
#define HTTP_MAX_HEAD_BYTES 8192      /* request line + headers  -> 431 */
#define HTTP_MAX_URI        2048      /* request-target buffer incl. NUL:
                                         targets of HTTP_MAX_URI-1 bytes or
                                         more                     -> 414 */
#define HTTP_MAX_HEADERS    64        /* header field count      -> 431 */
#define HTTP_MAX_BODY       (1 << 20) /* Content-Length          -> 413 */

typedef struct {
    char name[64];
    char value[1024];
} http_header;

typedef struct {
    char method[16];          /* "GET", "POST", "HEAD" ... */
    char uri[HTTP_MAX_URI];   /* raw request-target, as sent */
    char path[HTTP_MAX_URI];  /* percent-decoded path, no query ("/a b.html") */
    char query[HTTP_MAX_URI]; /* raw query string without '?', "" if none */
    int version_minor;        /* 0 for HTTP/1.0, 1 for HTTP/1.1 */
    http_header headers[HTTP_MAX_HEADERS];
    size_t header_count;
    int has_content_length;
    size_t content_length;
    int keep_alive;           /* derived from version + Connection header */
    char *body;               /* malloc'd, NUL-terminated, NULL if none */
    size_t body_len;
} http_request;

typedef enum {
    PARSE_OK = 0,
    PARSE_INCOMPLETE,            /* need more bytes (no CRLFCRLF yet) */
    PARSE_BAD_REQUEST,           /* 400 */
    PARSE_URI_TOO_LONG,          /* 414 */
    PARSE_HEADERS_TOO_LARGE,     /* 431 */
    PARSE_BODY_TOO_LARGE,        /* 413 */
    PARSE_NOT_IMPLEMENTED,       /* 501: unknown method */
    PARSE_VERSION_NOT_SUPPORTED  /* 505 */
} parse_result;

/* Parse the request head contained in buf[0..len).
 * On PARSE_OK fills req (body stays NULL) and sets *head_len to the number
 * of bytes up to and including the terminating CRLFCRLF.
 * Pure function: no I/O, no allocation. Unit-testable. */
parse_result http_parse_head(const char *buf, size_t len, http_request *req,
                             size_t *head_len);

/* Map a parse_result to its HTTP status code (PARSE_OK -> 200). */
int http_parse_status(parse_result r);

/* Case-insensitive header lookup. Returns NULL if absent. */
const char *http_get_header(const http_request *req, const char *name);

/* Decode %XX and reject invalid escapes / embedded NUL. Returns 0 on success. */
int http_url_decode(const char *src, char *dst, size_t dst_size);

void http_request_free(http_request *req);

/* Per-connection read state. Bytes read past the end of one request
 * (pipelining) stay in buf for the next call. */
typedef struct {
    int fd;
    char buf[HTTP_MAX_HEAD_BYTES];
    size_t len;
} http_conn;

/* Read one complete request (head + Content-Length body) from c->fd.
 * Returns 0 on success, -1 if the peer closed / timed out before sending
 * anything, or an HTTP status code (400, 408, 413, 414, 431, 501, 505)
 * the caller should answer with before closing. */
int http_read_request(http_conn *c, http_request *req);

/* ---- responses ---- */

ssize_t write_all(int fd, const void *buf, size_t n);
const char *http_status_text(int status);

/* Send a full response. extra_headers is either NULL or a block of
 * "Name: value\r\n" lines. If head_only, the body is not sent.
 * Returns 0 on success, -1 on write error. */
int http_send_response(int fd, int status, const char *content_type,
                       const char *body, size_t body_len, int keep_alive,
                       const char *extra_headers, int head_only);

/* Small HTML error page for status. */
int http_send_error(int fd, int status, int keep_alive);

#endif
