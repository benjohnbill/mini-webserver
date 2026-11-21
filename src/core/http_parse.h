#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include <stdbool.h>
#include <stddef.h>

/* Shared core. struct request points into the caller's buffer. */
struct request {
    char *method;
    char *target;
    char *version;
    char *host;          /* NULL if absent */
    char *content_type;  /* NULL if absent; POST /echo sends it back */
    long content_length; /* -1 if absent */
    bool wants_close;    /* from the version and Connection */
    char *body;
    size_t body_len;
};

#define HTTP_INCOMPLETE (-1)

/* Returns 0 when the request is complete, HTTP_INCOMPLETE when more bytes
   are needed, or a status code (400, 405, 411, 413, 414, 431, 505). */
int http_parse_request(char *buffer, size_t len, struct request *request);

/* Returns HTTP_INCOMPLETE, the total length of the request (header and
   body) as soon as the header is complete, or a status code as a negative
   number (-400). The sign is necessary because a length can be equal to a
   status code. Does not change the buffer. */
long http_frame_measure(const char *buffer, size_t len);

#endif
