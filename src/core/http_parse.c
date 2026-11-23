#include "http_parse.h"

#include <string.h>
#include <strings.h>

#include "config.h"

struct header_fields {
    int content_length_count;
    const char *content_length;
    bool has_transfer_encoding;
    bool has_close;
    bool has_keep_alive;
};

static size_t find_header_end(const char *buffer, size_t len);
static long partial_frame_measure(const char *buffer, size_t scan_len,
                                  size_t len);
static int head_parse(char *buffer, size_t header_len, struct request *request);
static int start_line_parse(char *buffer, size_t len, struct request *request,
                            char **next_line);
static char *end_first_line(char *buffer);
static int parse_request_line(char *buffer, struct request *request);
static char *split_at_space(char *text);
static bool is_target_too_long(const char *target);
static int header_lines_parse(char *line, struct request *request);
static bool line_is_empty(const char *line);
static int header_line_parse(char *line, struct request *request,
                             struct header_fields *fields);
static char *text_trim_spaces(char *text);
static void header_fields_record(const char *name, char *value,
                                 struct request *request,
                                 struct header_fields *fields);
static int request_line_check(const struct request *request);
static bool is_supported_version(const char *version);
static bool is_supported_method(const char *method);
static int body_framing_check(struct request *request,
                              const struct header_fields *fields);
static int content_length_parse(const char *value, long *content_length);

int http_parse_request(char *buffer, size_t len, struct request *request)
{
    long frame = http_frame_measure(buffer, len);
    if (frame == HTTP_INCOMPLETE) {
        return HTTP_INCOMPLETE;
    }
    if (frame < 0) {
        return (int)-frame;
    }
    if ((size_t)frame > len) {
        return HTTP_INCOMPLETE;
    }

    size_t scan_len = len < HEADER_MAX ? len : HEADER_MAX;
    size_t header_len = find_header_end(buffer, scan_len);
    int status = head_parse(buffer, header_len, request);
    if (status != 0) {
        return status;
    }
    request->body = buffer + header_len;
    request->body_len = (size_t)frame - header_len;

    return 0;
}

long http_frame_measure(const char *buffer, size_t len)
{
    char copy[HEADER_MAX + 1];
    struct request request;
    size_t scan_len = len < HEADER_MAX ? len : HEADER_MAX;
    size_t header_len = find_header_end(buffer, scan_len);
    if (header_len == 0) {
        return partial_frame_measure(buffer, scan_len, len);
    }

    memcpy(copy, buffer, header_len);
    copy[header_len] = '\0';
    int status = head_parse(copy, header_len, &request);
    if (status != 0) {
        return -status;
    }
    long body_len = request.content_length > 0 ? request.content_length : 0;

    return (long)header_len + body_len;
}

/* Returns the length of the header section, or 0 if it has no end yet. */
static size_t find_header_end(const char *buffer, size_t len)
{
    const char *cursor = memchr(buffer, '\n', len);
    while (cursor != NULL) {
        size_t offset = (size_t)(cursor - buffer);
        size_t rest = len - offset - 1;
        if (rest >= 1 && cursor[1] == '\n') {
            return offset + 2;
        }
        if (rest >= 2 && cursor[1] == '\r' && cursor[2] == '\n') {
            return offset + 3;
        }
        cursor = memchr(cursor + 1, '\n', rest);
    }

    return 0;
}

/* The header has no end yet. A finished request line can still decide 400
   (bad syntax) or 414 (SPEC §6: 414 when the request line ends). 505 and 405
   wait for the header end, because a bad header line gets 400 first. */
static long partial_frame_measure(const char *buffer, size_t scan_len,
                                  size_t len)
{
    const char *line_end = memchr(buffer, '\n', scan_len);
    if (line_end != NULL) {
        char copy[HEADER_MAX + 1];
        struct request request;
        char *next_line = NULL;
        size_t line_len = (size_t)(line_end - buffer) + 1;
        memcpy(copy, buffer, line_len);
        copy[line_len] = '\0';
        int status = start_line_parse(copy, line_len, &request, &next_line);
        if (status != 0) {
            return -status;
        }
        if (is_target_too_long(request.target)) {
            return -414;
        }
    }

    return len >= HEADER_MAX ? -431 : HTTP_INCOMPLETE;
}

static int head_parse(char *buffer, size_t header_len, struct request *request)
{
    char *line = NULL;
    int status = start_line_parse(buffer, header_len, request, &line);
    if (status != 0) {
        return status;
    }

    return header_lines_parse(line, request);
}

static int start_line_parse(char *buffer, size_t len, struct request *request,
                            char **next_line)
{
    *request = (struct request){.content_length = -1};
    if (memchr(buffer, '\0', len) != NULL) {
        return 400;
    }
    *next_line = end_first_line(buffer);
    if (*next_line == NULL) {
        return 400;
    }

    return parse_request_line(buffer, request);
}

/* Returns the start of the next line, or NULL if there is no line end. */
static char *end_first_line(char *buffer)
{
    char *line_end = strchr(buffer, '\n');
    if (line_end == NULL) {
        return NULL;
    }
    *line_end = '\0';
    if (line_end > buffer && line_end[-1] == '\r') {
        line_end[-1] = '\0';
    }

    return line_end + 1;
}

static int parse_request_line(char *buffer, struct request *request)
{
    request->method = buffer;
    request->target = split_at_space(request->method);
    request->version = split_at_space(request->target);

    if (request->version == NULL || request->method[0] == '\0' ||
        request->target[0] != '/' || strchr(request->version, ' ') != NULL) {
        return 400;
    }

    return 0;
}

static char *split_at_space(char *text)
{
    if (text == NULL) {
        return NULL;
    }
    char *space = strchr(text, ' ');
    if (space == NULL) {
        return NULL;
    }
    *space = '\0';

    return space + 1;
}

static bool is_target_too_long(const char *target)
{
    return strlen(target) > TARGET_MAX;
}

static int header_lines_parse(char *line, struct request *request)
{
    struct header_fields fields = {0};
    while (!line_is_empty(line)) {
        char *next_line = end_first_line(line);
        if (next_line == NULL) {
            return 400;
        }
        int status = header_line_parse(line, request, &fields);
        if (status != 0) {
            return status;
        }
        line = next_line;
    }
    int status = request_line_check(request);
    if (status != 0) {
        return status;
    }

    bool is_http_1_0 = strcmp(request->version, "HTTP/1.0") == 0;
    request->wants_close =
        is_http_1_0 ? !fields.has_keep_alive : fields.has_close;
    /* SLOT(owner): Host check, HTTP/1.1 only */

    return body_framing_check(request, &fields);
}

static bool line_is_empty(const char *line)
{
    return line[0] == '\n' || (line[0] == '\r' && line[1] == '\n');
}

static int header_line_parse(char *line, struct request *request,
                             struct header_fields *fields)
{
    char *colon = strchr(line, ':');
    if (colon == NULL || colon == line) {
        return 400;
    }
    *colon = '\0';
    /* RFC 9112 section 5.1: no whitespace between the name and the colon. */
    if (strpbrk(line, " \t") != NULL) {
        return 400;
    }
    header_fields_record(line, text_trim_spaces(colon + 1), request, fields);

    return 0;
}

static char *text_trim_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }

    return text;
}

static void header_fields_record(const char *name, char *value,
                                 struct request *request,
                                 struct header_fields *fields)
{
    if (strcasecmp(name, "Host") == 0) {
        request->host = value;
    } else if (strcasecmp(name, "Content-Type") == 0) {
        request->content_type = value;
    } else if (strcasecmp(name, "User-Agent") == 0) {
        request->user_agent = value;
    } else if (strcasecmp(name, "Content-Length") == 0) {
        fields->content_length_count++;
        fields->content_length = value;
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
        fields->has_transfer_encoding = true;
    } else if (strcasecmp(name, "Connection") == 0) {
        fields->has_close |= strcasecmp(value, "close") == 0;
        fields->has_keep_alive |= strcasecmp(value, "keep-alive") == 0;
    }
}

/* SPEC §3: after the syntax checks, 505, then 405, then 414. */
static int request_line_check(const struct request *request)
{
    if (!is_supported_version(request->version)) {
        return 505;
    }
    if (!is_supported_method(request->method)) {
        return 405;
    }
    if (is_target_too_long(request->target)) {
        return 414;
    }

    return 0;
}

static bool is_supported_version(const char *version)
{
    return strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0;
}

static bool is_supported_method(const char *method)
{
    return strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0;
}

static int body_framing_check(struct request *request,
                              const struct header_fields *fields)
{
    if (fields->has_transfer_encoding || fields->content_length_count > 1) {
        return 400;
    }
    if (fields->content_length_count == 1) {
        int status = content_length_parse(fields->content_length,
                                          &request->content_length);
        if (status != 0) {
            return status;
        }
    }
    if (request->content_length < 0 && strcmp(request->method, "POST") == 0) {
        return 411;
    }

    return 0;
}

static int content_length_parse(const char *value, long *content_length)
{
    long number = 0;
    if (value[0] == '\0') {
        return 400;
    }
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return 400;
        }
        /* Stop growing past the limit, so a long value cannot overflow. */
        if (number <= BODY_MAX) {
            number = number * 10 + (*cursor - '0');
        }
    }
    if (number > BODY_MAX) {
        return 413;
    }
    *content_length = number;

    return 0;
}
