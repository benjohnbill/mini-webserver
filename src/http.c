/* HTTP/1.x request parsing and response writing.
 *
 * The parser is deliberately strict (RFC 7230 section 3): anything that is
 * not clearly a well-formed request is rejected rather than guessed at,
 * because lenient parsing is where request-smuggling bugs come from. */
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

/* RFC 7230 tchar: the characters allowed in a token (method, header name). */
static int is_tchar(unsigned char ch)
{
    if (isalnum(ch))
        return 1;
    return ch != '\0' && strchr("!#$%&'*+-.^_`|~", ch) != NULL;
}

static int is_ows(char ch) { return ch == ' ' || ch == '\t'; }

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Find "\r\n" in [p, end). Returns pointer to '\r' or NULL. */
static const char *find_crlf(const char *p, const char *end)
{
    for (; p + 1 < end; p++)
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    return NULL;
}

/* Find the CRLFCRLF that terminates the head. */
static const char *find_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    return NULL;
}

/* Copy [src, src+n) into dst as a C string. Returns -1 if it does not fit. */
static int copy_span(char *dst, size_t dst_size, const char *src, size_t n)
{
    if (n >= dst_size)
        return -1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}

int http_url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    if (dst_size == 0)
        return -1;
    for (size_t i = 0; src[i] != '\0'; i++) {
        char ch = src[i];
        if (ch == '%') {
            int hi = hex_value(src[i + 1]);
            int lo = hi < 0 ? -1 : hex_value(src[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;              /* "%zz", "%4" or "%" at end */
            ch = (char)(hi * 16 + lo);
            if (ch == '\0')
                return -1;              /* %00 would truncate the path */
            i += 2;
        }
        if (o + 1 >= dst_size)
            return -1;
        dst[o++] = ch;
    }
    dst[o] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* request line                                                        */
/* ------------------------------------------------------------------ */

/* Split a target into path (decoded) and query. For absolute-form
 * ("http://host/p?q") the path is the part after the authority. */
static parse_result split_target(http_request *req)
{
    const char *t = req->uri;
    const char *path_start;

    if (t[0] == '/') {
        path_start = t;
    } else if (strncasecmp(t, "http://", 7) == 0) {
        path_start = t + 7 + strcspn(t + 7, "/?#");
        if (path_start == t + 7)
            return PARSE_BAD_REQUEST;   /* empty authority */
    } else {
        return PARSE_BAD_REQUEST;       /* "*", authority-form, garbage */
    }

    size_t path_len = strcspn(path_start, "?#");
    const char *q = path_start + path_len;
    char raw_path[HTTP_MAX_URI];

    if (path_len == 0) {
        strcpy(raw_path, "/");          /* "http://host" or "http://host?q" */
    } else if (copy_span(raw_path, sizeof raw_path, path_start, path_len) < 0) {
        return PARSE_URI_TOO_LONG;
    }

    if (*q == '?') {
        size_t qlen = strcspn(q + 1, "#");
        if (copy_span(req->query, sizeof req->query, q + 1, qlen) < 0)
            return PARSE_URI_TOO_LONG;
    }

    if (http_url_decode(raw_path, req->path, sizeof req->path) != 0)
        return PARSE_BAD_REQUEST;
    return PARSE_OK;
}

/* "METHOD SP request-target SP HTTP/1.x" (without the CRLF). */
static parse_result parse_request_line(const char *p, const char *end,
                                       http_request *req)
{
    /* method */
    const char *m = p;
    while (p < end && is_tchar((unsigned char)*p))
        p++;
    if (p == m || p == end || *p != ' ')
        return PARSE_BAD_REQUEST;
    size_t mlen = (size_t)(p - m);
    int known = 0;
    if (mlen < sizeof req->method) {
        memcpy(req->method, m, mlen);
        req->method[mlen] = '\0';
        known = strcmp(req->method, "GET") == 0 ||
                strcmp(req->method, "POST") == 0 ||
                strcmp(req->method, "HEAD") == 0;
    }
    p++;                                /* exactly one SP */

    /* request-target: visible ASCII only, no spaces or controls */
    const char *t = p;
    while (p < end && *p != ' ') {
        unsigned char ch = (unsigned char)*p;
        if (ch <= 0x20 || ch >= 0x7f)
            return PARSE_BAD_REQUEST;
        p++;
    }
    if (p == t || p == end)
        return PARSE_BAD_REQUEST;
    if (copy_span(req->uri, sizeof req->uri, t, (size_t)(p - t)) < 0)
        return PARSE_URI_TOO_LONG;
    p++;

    /* HTTP-version = "HTTP/" DIGIT "." DIGIT, then end of line */
    if (end - p != 8 || memcmp(p, "HTTP/", 5) != 0 ||
        !isdigit((unsigned char)p[5]) || p[6] != '.' ||
        !isdigit((unsigned char)p[7]))
        return PARSE_BAD_REQUEST;
    if (p[5] != '1' || (p[7] != '0' && p[7] != '1'))
        return PARSE_VERSION_NOT_SUPPORTED;
    req->version_minor = p[7] - '0';

    /* Syntax is fine; only now decide whether we implement the method. */
    if (!known)
        return PARSE_NOT_IMPLEMENTED;
    return split_target(req);
}

/* ------------------------------------------------------------------ */
/* headers                                                             */
/* ------------------------------------------------------------------ */

static parse_result parse_header_line(const char *p, const char *end,
                                      http_request *req)
{
    /* obs-fold (continuation line) is deprecated; reject it. */
    if (is_ows(*p))
        return PARSE_BAD_REQUEST;

    const char *name = p;
    while (p < end && is_tchar((unsigned char)*p))
        p++;
    /* No ':' or whitespace before ':' -> 400 (RFC 7230 3.2.4). */
    if (p == name || p == end || *p != ':')
        return PARSE_BAD_REQUEST;
    size_t nlen = (size_t)(p - name);
    p++;

    while (p < end && is_ows(*p))
        p++;
    const char *vend = end;
    while (vend > p && is_ows(vend[-1]))
        vend--;
    for (const char *v = p; v < vend; v++) {
        unsigned char ch = (unsigned char)*v;
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f)
            return PARSE_BAD_REQUEST;   /* bare CR/LF, NUL, controls */
    }

    if (req->header_count >= HTTP_MAX_HEADERS)
        return PARSE_HEADERS_TOO_LARGE;
    http_header *h = &req->headers[req->header_count];
    if (copy_span(h->name, sizeof h->name, name, nlen) < 0 ||
        copy_span(h->value, sizeof h->value, p, (size_t)(vend - p)) < 0)
        return PARSE_HEADERS_TOO_LARGE;
    req->header_count++;
    return PARSE_OK;
}

/* Content-Length = 1*DIGIT. A list like "5, 5" is rejected outright. */
static parse_result parse_content_length(const char *v, size_t *out)
{
    size_t n = 0;
    if (*v == '\0')
        return PARSE_BAD_REQUEST;
    for (; *v; v++) {
        if (!isdigit((unsigned char)*v))
            return PARSE_BAD_REQUEST;
        n = n * 10 + (size_t)(*v - '0');
        if (n > HTTP_MAX_BODY)
            return PARSE_BODY_TOO_LARGE; /* also stops overflow early */
    }
    *out = n;
    return PARSE_OK;
}

/* Apply the semantics of the headers we care about. */
static parse_result interpret_headers(http_request *req)
{
    int host_count = 0;
    int saw_close = 0, saw_keep_alive = 0;

    for (size_t i = 0; i < req->header_count; i++) {
        const http_header *h = &req->headers[i];
        if (strcasecmp(h->name, "Host") == 0) {
            host_count++;
        } else if (strcasecmp(h->name, "Content-Length") == 0) {
            size_t n = 0;
            parse_result r = parse_content_length(h->value, &n);
            if (r != PARSE_OK)
                return r;
            /* Two different lengths = ambiguous framing -> smuggling risk. */
            if (req->has_content_length && req->content_length != n)
                return PARSE_BAD_REQUEST;
            req->has_content_length = 1;
            req->content_length = n;
        } else if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            return PARSE_NOT_IMPLEMENTED; /* no chunked request bodies */
        } else if (strcasecmp(h->name, "Connection") == 0) {
            /* Comma-separated token list, e.g. "keep-alive, Upgrade". */
            const char *s = h->value;
            while (*s) {
                while (*s == ',' || is_ows(*s))
                    s++;
                size_t tl = strcspn(s, ",");
                size_t trimmed = tl;
                while (trimmed > 0 && is_ows(s[trimmed - 1]))
                    trimmed--;
                if (trimmed == 5 && strncasecmp(s, "close", 5) == 0)
                    saw_close = 1;
                else if (trimmed == 10 && strncasecmp(s, "keep-alive", 10) == 0)
                    saw_keep_alive = 1;
                s += tl;
            }
        }
    }

    /* RFC 7230 5.4: HTTP/1.1 requests MUST carry exactly one Host. */
    if (host_count > 1 || (req->version_minor == 1 && host_count == 0))
        return PARSE_BAD_REQUEST;

    /* 1.1 is persistent by default, 1.0 only on explicit request;
     * "close" always wins. */
    req->keep_alive = req->version_minor == 1;
    if (saw_keep_alive)
        req->keep_alive = 1;
    if (saw_close)
        req->keep_alive = 0;
    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* public parser API                                                   */
/* ------------------------------------------------------------------ */

parse_result http_parse_head(const char *buf, size_t len, http_request *req,
                             size_t *head_len)
{
    memset(req, 0, sizeof *req);

    /* RFC 7230 3.5: ignore empty lines before the request line (some
     * clients send an extra CRLF after a POST body). */
    size_t skip = 0;
    while (skip + 1 < len && buf[skip] == '\r' && buf[skip + 1] == '\n')
        skip += 2;

    const char *start = buf + skip;
    const char *end_all = buf + len;
    const char *head_end = find_head_end(start, len - skip);

    if (head_end == NULL) {
        if (len < HTTP_MAX_HEAD_BYTES)
            return PARSE_INCOMPLETE;
        /* Buffer is full and the head is still open: report the more
         * specific 414 when the request line alone is already too long. */
        const char *eol = find_crlf(start, end_all);
        size_t line_len = eol ? (size_t)(eol - start) : (size_t)(end_all - start);
        return line_len > HTTP_MAX_URI ? PARSE_URI_TOO_LONG
                                       : PARSE_HEADERS_TOO_LARGE;
    }
    size_t total = (size_t)(head_end - buf) + 4;
    if (total > HTTP_MAX_HEAD_BYTES)
        return PARSE_HEADERS_TOO_LARGE;

    /* head_end points at the final "\r\n\r\n"; lines end with "\r\n".
     * Treat the head as [start, head_end + 2) so every line has a CRLF. */
    const char *end = head_end + 2;
    const char *eol = find_crlf(start, end);
    if (memchr(start, '\0', (size_t)(end - start)) != NULL)
        return PARSE_BAD_REQUEST;

    parse_result r = parse_request_line(start, eol, req);
    if (r != PARSE_OK)
        return r;

    for (const char *p = eol + 2; p < end; p = eol + 2) {
        eol = find_crlf(p, end);
        r = parse_header_line(p, eol, req);
        if (r != PARSE_OK)
            return r;
    }

    r = interpret_headers(req);
    if (r != PARSE_OK)
        return r;
    *head_len = total;
    return PARSE_OK;
}

int http_parse_status(parse_result r)
{
    switch (r) {
    case PARSE_OK:                    return 200;
    case PARSE_INCOMPLETE:            return 400;
    case PARSE_BAD_REQUEST:           return 400;
    case PARSE_URI_TOO_LONG:          return 414;
    case PARSE_HEADERS_TOO_LARGE:     return 431;
    case PARSE_BODY_TOO_LARGE:        return 413;
    case PARSE_NOT_IMPLEMENTED:       return 501;
    case PARSE_VERSION_NOT_SUPPORTED: return 505;
    }
    return 500;
}

const char *http_get_header(const http_request *req, const char *name)
{
    for (size_t i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

void http_request_free(http_request *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

/* ------------------------------------------------------------------ */
/* reading from the socket                                             */
/* ------------------------------------------------------------------ */

/* recv() that retries on EINTR. The caller sets SO_RCVTIMEO, so a timeout
 * shows up as -1 with EAGAIN/EWOULDBLOCK. */
static ssize_t recv_retry(int fd, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

static int is_timeout(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }

static long elapsed_ms(const struct timespec *since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - since->tv_sec) * 1000 +
           (now.tv_nsec - since->tv_nsec) / 1000000;
}

/* Upper bound for receiving a whole head once its first byte arrived.
 * SO_RCVTIMEO alone only limits each recv(), so a client trickling one
 * byte per second ("slowloris") could otherwise hold a worker forever. */
static long head_deadline_ms(int fd)
{
    struct timeval tv;
    socklen_t sl = sizeof tv;
    long ms = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &sl) == 0)
        ms = (long)tv.tv_sec * 1000 + (long)tv.tv_usec / 1000;
    return ms > 0 ? ms * 2 : 10000;
}

int http_read_request(http_conn *c, http_request *req)
{
    size_t head_len = 0;
    struct timespec started;
    long deadline = head_deadline_ms(c->fd);
    int timing = 0;

    memset(req, 0, sizeof *req);
    if (c->len > 0) {
        clock_gettime(CLOCK_MONOTONIC, &started);
        timing = 1;
    }

    /* 1. accumulate until a complete head is buffered */
    for (;;) {
        if (c->len > 0) {
            parse_result r = http_parse_head(c->buf, c->len, req, &head_len);
            if (r == PARSE_OK)
                break;
            if (r != PARSE_INCOMPLETE)
                return http_parse_status(r);
            if (timing && elapsed_ms(&started) > deadline)
                return 408;
        }
        ssize_t n = recv_retry(c->fd, c->buf + c->len, sizeof c->buf - c->len);
        if (n < 0) {
            /* Idle keep-alive timeout before any byte: close silently. */
            if (is_timeout() && c->len > 0)
                return 408;
            return -1;
        }
        if (n == 0)
            return -1;                  /* peer closed (maybe mid-head) */
        if (!timing) {
            clock_gettime(CLOCK_MONOTONIC, &started);
            timing = 1;
        }
        c->len += (size_t)n;
    }

    /* 2. drop the head from the buffer, keeping whatever followed it */
    memmove(c->buf, c->buf + head_len, c->len - head_len);
    c->len -= head_len;

    /* Expect (RFC 7231 5.1.1): only "100-continue" is defined, and it is
     * ignored for HTTP/1.0. The 413 check already happened in the parser,
     * so a client waiting for 100 only gets it if we will take the body. */
    const char *expect = http_get_header(req, "Expect");
    if (expect != NULL && req->version_minor == 1) {
        if (strcasecmp(expect, "100-continue") != 0)
            return 417;
        if (req->has_content_length && req->content_length > c->len) {
            static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
            if (write_all(c->fd, cont, sizeof cont - 1) < 0)
                return -1;
        }
    }

    if (!req->has_content_length || req->content_length == 0)
        return 0;

    /* 3. body: exactly content_length bytes, NUL-terminated for callers
     *    that treat it as text. Bytes already buffered come first. */
    size_t need = req->content_length;
    req->body = malloc(need + 1);
    if (req->body == NULL)
        return 500;
    size_t have = c->len < need ? c->len : need;
    memcpy(req->body, c->buf, have);
    memmove(c->buf, c->buf + have, c->len - have);
    c->len -= have;

    /* Same overall deadline as for the head, restarted for the body, so a
     * client trickling body bytes cannot hold the worker indefinitely. */
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (have < need) {
        if (elapsed_ms(&started) > deadline) {
            http_request_free(req);
            return 408;
        }
        ssize_t n = recv_retry(c->fd, req->body + have, need - have);
        if (n <= 0) {
            int status = (n < 0 && is_timeout()) ? 408 : -1;
            http_request_free(req);
            return status;
        }
        have += (size_t)n;
    }
    req->body[need] = '\0';
    req->body_len = need;
    return 0;
}

/* ------------------------------------------------------------------ */
/* responses                                                           */
/* ------------------------------------------------------------------ */

ssize_t write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;
#ifdef MSG_NOSIGNAL
    int use_send = 1;
#endif
    while (done < n) {
        ssize_t w;
#ifdef MSG_NOSIGNAL
        /* send() lets us suppress SIGPIPE per call; fall back to write()
         * for non-socket fds (pipes in CGI, files in tests). */
        if (use_send) {
            w = send(fd, p + done, n - done, MSG_NOSIGNAL);
            if (w < 0 && errno == ENOTSOCK) {
                use_send = 0;
                continue;
            }
        } else
#endif
            w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return (ssize_t)done;
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* Responses up to this size go out in a single write so that small
 * keep-alive replies are not split across two TCP segments. */
#define COALESCE_LIMIT (64 * 1024)

int http_send_response(int fd, int status, const char *content_type,
                       const char *body, size_t body_len, int keep_alive,
                       const char *extra_headers, int head_only)
{
    char date[64] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (gmtime_r(&now, &tm) != NULL)
        strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    /* 1xx, 204 and 304 never carry a body (RFC 7230 3.3.3), and a server
     * must not send Content-Length with 1xx/204. */
    int no_body = (status >= 100 && status < 200) || status == 204 ||
                  status == 304;
    char clen[48] = "";
    if (!no_body)
        snprintf(clen, sizeof clen, "Content-Length: %zu\r\n", body_len);

    char head[4096];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Date: %s\r\n"
                      "Server: mini-webserver\r\n"
                      "Content-Type: %s\r\n"
                      "%s"
                      "Connection: %s\r\n"
                      "%s"
                      "\r\n",
                      status, http_status_text(status), date,
                      content_type ? content_type : "application/octet-stream",
                      clen, keep_alive ? "keep-alive" : "close",
                      extra_headers ? extra_headers : "");
    if (hl < 0 || (size_t)hl >= sizeof head)
        return -1;                      /* extra_headers too large */

    size_t send_body = (head_only || body == NULL || no_body) ? 0 : body_len;
    if (send_body > 0 && (size_t)hl + send_body <= COALESCE_LIMIT) {
        char *msg = malloc((size_t)hl + send_body);
        if (msg != NULL) {
            memcpy(msg, head, (size_t)hl);
            memcpy(msg + hl, body, send_body);
            ssize_t w = write_all(fd, msg, (size_t)hl + send_body);
            free(msg);
            return w < 0 ? -1 : 0;
        }
    }
    if (write_all(fd, head, (size_t)hl) < 0)
        return -1;
    if (send_body > 0 && write_all(fd, body, send_body) < 0)
        return -1;
    return 0;
}

int http_send_error(int fd, int status, int keep_alive)
{
    char body[512];
    const char *text = http_status_text(status);
    int n = snprintf(body, sizeof body,
                     "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>mini-webserver</p></body></html>\n",
                     status, text, status, text);
    if (n < 0 || (size_t)n >= sizeof body)
        return -1;
    const char *extra = NULL;
    if (status == 503)
        extra = "Retry-After: 1\r\n";
    return http_send_response(fd, status, "text/html; charset=utf-8", body,
                              (size_t)n, keep_alive, extra, 0);
}
