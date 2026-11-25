#include "handlers.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

bool parse_port(const char *value, uint16_t *port)
{
    if (*value == '\0')
        return false;
    unsigned int number = 0;
    for (; *value; value++) {
        if (*value < '0' || *value > '9')
            return false;
        number = number * 10 + (unsigned int)(*value - '0');
        if (number > UINT16_MAX)
            return false;
    }
    if (number == 0)
        return false;
    *port = (uint16_t)number;
    return true;
}

bool parse_route(const char *value, Route *route)
{
    const char *equals = strchr(value, '=');
    if (equals == NULL || *value != '/')
        return false;

    size_t prefix_length = (size_t)(equals - value);
    const char tag[] = "@ketama";
    if (prefix_length > sizeof tag - 1 &&
        memcmp(equals - (sizeof tag - 1), tag, sizeof tag - 1) == 0)
        prefix_length -= sizeof tag - 1;
    if (prefix_length > HTTP_TARGET_LIMIT)
        return false;
    for (const unsigned char *p = (const unsigned char *)value;
         p < (const unsigned char *)equals; p++) {
        if (*p <= 32 || *p >= 127 || *p == '?' || *p == '#')
            return false;
    }
    while (prefix_length > 1 && value[prefix_length - 1] == '/')
        prefix_length--;

    const char *item = equals + 1;
    route->count = 0;
    route->ring = NULL;
    for (;;) {
        const char *end = item + strcspn(item, ",");
        const char *colon = memchr(item, ':', (size_t)(end - item));
        if (!colon || colon == item || (size_t)(colon - item) >= INET_ADDRSTRLEN ||
            route->count == BACKEND_LIMIT)
            return false;
        Backend *backend = &route->backends[route->count];
        size_t length = (size_t)(colon - item);
        memcpy(backend->ip, item, length);
        backend->ip[length] = 0;
        char port[16];
        size_t digits = (size_t)(end - colon - 1);
        if (digits >= sizeof port)
            return false;
        memcpy(port, colon + 1, digits);
        port[digits] = 0;
        struct in_addr ip;
        if (inet_pton(AF_INET, backend->ip, &ip) != 1 || !parse_port(port, &backend->port))
            return false;
        for (size_t i = 0; i < route->count; i++)
            if (route->backends[i].port == backend->port &&
                strcmp(route->backends[i].ip, backend->ip) == 0)
                return false;
        route->count++;
        if (!*end)
            break;
        item = end + 1;
    }
    if (route->count > 1 && !ketama_build(route))
        return false;
    memcpy(route->prefix, value, prefix_length);
    route->prefix[prefix_length] = '\0';
    return true;
}

const Route *match_route(const ServerConfig *config, const char *path)
{
    const Route *best = NULL;
    size_t longest = 0;
    for (size_t i = 0; i < config->route_count; i++) {
        const Route *route = &config->routes[i];
        size_t length = strlen(route->prefix);
        if (length > longest && strncmp(path, route->prefix, length) == 0 &&
            (length == 1 || path[length] == '\0' || path[length] == '/')) {
            best = route;
            longest = length;
        }
    }
    return best;
}

const char *mime_type_for(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (extension == NULL)
        return "application/octet-stream";

    const struct { const char *extension; const char *type; } types[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"}
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (strcasecmp(extension, types[i].extension) == 0)
            return types[i].type;
    }
    return "application/octet-stream";
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool decode_path(const char *source, char *target)
{
    while (*source) {
        unsigned char byte = (unsigned char)*source++;
        if (byte == '%') {
            if (source[0] == '\0' || source[1] == '\0')
                return false;
            int high = hex_digit((unsigned char)source[0]);
            int low = hex_digit((unsigned char)source[1]);
            if (high < 0 || low < 0)
                return false;
            byte = (unsigned char)(16 * high + low);
            source += 2;
        }
        if (byte < 32 || byte == 127 || byte == '\\')
            return false;
        *target++ = (char)byte;
    }
    *target = '\0';
    return true;
}

static int file_error(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
        return 404;
    default:
        return 500;
    }
}

static bool confined_path(const char *path)
{
    while (*path) {
        path += strspn(path, "/");
        size_t length = strcspn(path, "/");
        if ((length == 1 && path[0] == '.') ||
            (length == 2 && path[0] == '.' && path[1] == '.'))
            return false;
        path += length;
    }
    return true;
}

static int open_static_file(int root_fd, char *path, const char **name, int *status)
{
    if (!confined_path(path)) {
        *status = 403;
        return -1;
    }

    bool trailing_slash = path[strlen(path) - 1] == '/';
    int directory = root_fd;
    char *saved;
    char *part = strtok_r(path, "/", &saved);
    if (part == NULL) {
        int fd = openat(root_fd, "index.html", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        *name = "index.html";
        if (fd == -1)
            *status = file_error(errno);
        return fd;
    }

    for (;;) {
        char *next = strtok_r(NULL, "/", &saved);
        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
        if (next != NULL || trailing_slash)
            flags |= O_DIRECTORY;
        int fd = openat(directory, part, flags);
        if (fd == -1)
            *status = file_error(errno);
        if (directory != root_fd)
            close(directory);
        if (fd == -1 || next == NULL) {
            *name = part;
            return fd;
        }

        directory = fd;
        part = next;
    }
}

int open_static(int root_fd, const char *request_path, int *result_fd,
                size_t *length, const char **type)
{
    char path[HTTP_TARGET_LIMIT + 1];
    if (!decode_path(request_path, path))
        return 400;
    const char *name;
    int status = 500;
    int fd = open_static_file(root_fd, path, &name, &status);
    if (fd < 0)
        return status;
    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX)
        status = 500;
    else if (!S_ISREG(info.st_mode))
        status = 403;
    else {
        *result_fd = fd;
        *length = (size_t)info.st_size;
        *type = mime_type_for(name);
        return 0;
    }
    close(fd);
    return status;
}

int build_proxy_request(HttpOutput *out, const Backend *backend, const Request *request)
{
    out->length = 0;
    if (http_append(out, "%s %s HTTP/1.1\r\nHost: %s:%u\r\n", request->method,
                    request->target, backend->ip, (unsigned)backend->port) < 0)
        return -1;
    for (size_t i = 0; i < request->head.count; i++) {
        Header h = request->head.headers[i];
        if (strcasecmp(h.name, "Host") != 0 && http_forward_header(&request->head, h.name) &&
            http_append(out, "%s: %s\r\n", h.name, h.value) < 0)
            return -1;
    }
    return http_append(out, "Via: %s mini-webserver\r\nConnection: close\r\n"
                       "Content-Length: %zu\r\n\r\n", request->version + 5, request->head.length);
}

int build_proxy_response(HttpOutput *out, const UpstreamResponse *response)
{
    out->length = 0;
    if (http_append(out, "HTTP/1.1 %d %s\r\n", response->status, response->reason) < 0)
        return -1;
    for (size_t i = 0; i < response->head.count; i++) {
        Header h = response->head.headers[i];
        if (http_forward_header(&response->head, h.name) &&
            http_append(out, "%s: %s\r\n", h.name, h.value) < 0)
            return -1;
    }
    if (response->head.has_length && response->status != 204 &&
        http_append(out, "Content-Length: %zu\r\n", response->head.length) < 0)
        return -1;
    return http_append(out, "Connection: close\r\n\r\n");
}
