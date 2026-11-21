#include "http.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool is_token(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           (c != '\0' && strchr("!#$%&'*+-.^`_|~", c) != NULL);
}

static bool parse_size(const char *text, size_t *size)
{
    *size = 0;
    if (*text == '\0')
        return false;
    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '9')
            return false;
        size_t digit = (size_t)(*text - '0');
        if (*size > (SIZE_MAX - digit) / 10)
            return false;
        *size = *size * 10 + digit;
    }
    return true;
}

static int read_head_bytes(Stream *stream, HttpHead *head)
{
    size_t used = 0;
    if (stream->start == stream->end) {
        stream->start = 0;
        stream->end = 0;
    }
    while (used < HTTP_HEADER_LIMIT) {
        if (stream->start == stream->end) {
            ssize_t bytes = stream_read(stream, stream->buffer, sizeof stream->buffer);
            if (bytes < 0)
                return errno == ETIMEDOUT ? 408 : 400;
            if (bytes == 0)
                return used == 0 ? -1 : 400;
            stream->start = 0;
            stream->end = (size_t)bytes;
        }
        unsigned char byte = stream->buffer[stream->start++];
        if (byte == '\0')
            return 400;
        head->raw[used++] = (char)byte;
        if (used >= 4 && memcmp(head->raw + used - 4, "\r\n\r\n", 4) == 0) {
            head->raw[used] = '\0';
            return 0;
        }
    }
    return 431;
}

static bool valid_value(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p < 32 && *p != '\t') || *p == 127)
            return false;
    }
    return true;
}

static int read_head(Stream *stream, HttpHead *head)
{
    memset(head, 0, sizeof *head);
    int result = read_head_bytes(stream, head);
    if (result != 0)
        return result;

    head->first_line = head->raw;
    char *end = strstr(head->raw, "\r\n");
    *end = '\0';
    if (!valid_value(head->first_line))
        return 400;

    for (char *line = end + 2; !(*line == '\r' && line[1] == '\n'); line = end + 2) {
        end = strstr(line, "\r\n");
        *end = '\0';
        if (head->count == HTTP_HEADER_COUNT)
            return 431;
        char *colon = strchr(line, ':');
        if (colon == NULL || colon == line)
            return 400;
        *colon = '\0';
        for (char *p = line; *p; p++) {
            if (!is_token((unsigned char)*p))
                return 400;
        }
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *tail = end;
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = '\0';
        if (!valid_value(value))
            return 400;

        head->headers[head->count++] = (Header){line, value};
        if (strcasecmp(line, "Content-Length") == 0) {
            if (head->has_length || !parse_size(value, &head->length))
                return 400;
            head->has_length = true;
        }
        if (strcasecmp(line, "Transfer-Encoding") == 0)
            head->has_transfer_encoding = true;
    }

    return head->has_length && head->has_transfer_encoding ? 400 : 0;
}

ssize_t http_read_body(Stream *stream, void *buffer, size_t size)
{
    size_t queued = stream->end - stream->start;
    if (queued == 0)
        return stream_read(stream, buffer, size);
    if (size > queued)
        size = queued;
    memcpy(buffer, stream->buffer + stream->start, size);
    stream->start += size;
    return (ssize_t)size;
}

static bool supported_version(const char *version)
{
    return strcmp(version, "HTTP/1.1") == 0 || strcmp(version, "HTTP/1.0") == 0;
}

int http_read_request(Stream *stream, Request *request)
{
    int result = read_head(stream, &request->head);
    if (result != 0)
        return result;

    request->method = request->head.first_line;
    request->target = strchr(request->method, ' ');
    if (request->target == NULL || request->target == request->method)
        return 400;
    *request->target++ = '\0';
    request->version = strchr(request->target, ' ');
    if (request->version == NULL || *request->target != '/')
        return 400;
    *request->version++ = '\0';
    if (strlen(request->target) > HTTP_TARGET_LIMIT)
        return 414;
    for (char *p = request->method; *p; p++) {
        if (!is_token((unsigned char)*p))
            return 400;
    }
    for (const unsigned char *p = (const unsigned char *)request->target; *p; p++) {
        if (*p <= 32 || *p >= 127 || *p == '#')
            return 400;
    }
    if (!supported_version(request->version)) {
        const char *version = request->version;
        if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 ||
            version[5] < '0' || version[5] > '9' || version[6] != '.' ||
            version[7] < '0' || version[7] > '9')
            return 400;
        return 505;
    }

    size_t host_count = 0;
    bool expect = false;
    for (size_t i = 0; i < request->head.count; i++) {
        Header header = request->head.headers[i];
        if (strcasecmp(header.name, "Host") == 0) {
            if (*header.value == '\0' || strpbrk(header.value, " \t,") != NULL)
                return 400;
            host_count++;
        }
        if (strcasecmp(header.name, "Expect") == 0)
            expect = true;
    }
    if (host_count > 1 || (strcmp(request->version, "HTTP/1.1") == 0 && host_count != 1))
        return 400;
    if (request->head.has_transfer_encoding)
        return 501;
    if (expect)
        return 417;
    if (request->head.length > HTTP_BODY_LIMIT)
        return 413;

    size_t path_length = strcspn(request->target, "?");
    request->path = strndup(request->target, path_length);
    if (request->path == NULL)
        return 500;

    if (request->head.length > 0) {
        request->body = malloc(request->head.length);
        if (request->body == NULL)
            return 500;
    }
    size_t received = 0;
    while (received < request->head.length) {
        ssize_t bytes = http_read_body(stream, request->body + received,
                                       request->head.length - received);
        if (bytes <= 0)
            return bytes < 0 && errno == ETIMEDOUT ? 408 : 400;
        received += (size_t)bytes;
    }
    return 0;
}

int http_read_response(Stream *stream, UpstreamResponse *response)
{
    int result = read_head(stream, &response->head);
    if (result != 0)
        return result == 408 ? 504 : 502;

    char *version = response->head.first_line;
    char *status = strchr(version, ' ');
    if (status == NULL)
        return 502;
    *status++ = '\0';
    if (!supported_version(version) || strlen(status) < 4 || status[3] != ' ')
        return 502;
    if (status[0] < '1' || status[0] > '5' || status[1] < '0' ||
        status[1] > '9' || status[2] < '0' || status[2] > '9')
        return 502;
    response->status = 100 * (status[0] - '0') + 10 * (status[1] - '0') + status[2] - '0';
    response->reason = status + 4;
    return 0;
}

bool http_forward_header(const HttpHead *head, const char *name)
{
    const char *const local[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer",
        "Transfer-Encoding", "Upgrade", "Proxy-Authorization", "Proxy-Authenticate",
        "Content-Length"
    };
    for (size_t i = 0; i < sizeof local / sizeof local[0]; i++) {
        if (strcasecmp(name, local[i]) == 0)
            return false;
    }
    for (size_t i = 0; i < head->count; i++) {
        if (strcasecmp(head->headers[i].name, "Connection") != 0)
            continue;

        const char *p = head->headers[i].value;
        while (*p) {
            p += strspn(p, " \t,");
            size_t length = strcspn(p, "\t ,");
            if (strlen(name) == length && strncasecmp(name, p, length) == 0)
                return false;
            p += length;
        }
    }
    return true;
}

int http_send_line(Stream *stream, const char *name, const char *value)
{
    if (stream_send(stream, name, strlen(name)) == -1 ||
        stream_send(stream, ": ", 2) == -1 ||
        stream_send(stream, value, strlen(value)) == -1 ||
        stream_send(stream, "\r\n", 2) == -1)
        return -1;
    return 0;
}

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

int http_send_head(Stream *stream, int status, const char *type, size_t length,
                   const char *allow)
{
    char header[512];
    int bytes = snprintf(header, sizeof header,
                         "HTTP/1.1 %d %s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: close\r\n"
                         "%s%s%s\r\n",
                         status, reason_phrase(status), type, length,
                         allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "");
    if (bytes < 0 || (size_t)bytes >= sizeof header)
        return -1;
    return stream_send(stream, header, (size_t)bytes);
}

int http_send_error(Stream *stream, int status, const char *allow)
{
    char body[128];
    int bytes = snprintf(body, sizeof body, "%d %s\n", status, reason_phrase(status));
    if (bytes < 0 || (size_t)bytes >= sizeof body ||
        http_send_head(stream, status, "text/plain; charset=utf-8", (size_t)bytes, allow) == -1)
        return -1;
    return stream_send(stream, body, (size_t)bytes);
}
