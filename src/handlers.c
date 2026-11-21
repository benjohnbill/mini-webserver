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
    if (prefix_length > HTTP_TARGET_LIMIT)
        return false;
    for (const unsigned char *p = (const unsigned char *)value;
         p < (const unsigned char *)equals; p++) {
        if (*p <= 32 || *p >= 127 || *p == '?' || *p == '#')
            return false;
    }
    while (prefix_length > 1 && value[prefix_length - 1] == '/')
        prefix_length--;

    const char *ip = equals + 1;
    const char *colon = strchr(ip, ':');
    if (colon == NULL || colon == ip || (size_t)(colon - ip) >= sizeof route->ip)
        return false;
    size_t ip_length = (size_t)(colon - ip);
    memcpy(route->ip, ip, ip_length);
    route->ip[ip_length] = '\0';
    struct in_addr address;
    if (inet_pton(AF_INET, route->ip, &address) != 1 || !parse_port(colon + 1, &route->port))
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

static void handle_static(Stream *client, int root_fd, const Request *request)
{
    char path[HTTP_TARGET_LIMIT + 1];
    if (!decode_path(request->path, path)) {
        http_send_error(client, 400, NULL);
        return;
    }

    const char *name;
    int error_status;
    int fd = open_static_file(root_fd, path, &name, &error_status);
    if (fd == -1) {
        http_send_error(client, error_status, NULL);
        return;
    }

    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) {
        http_send_error(client, 500, NULL);
    } else if (!S_ISREG(info.st_mode)) {
        http_send_error(client, 403, NULL);
    } else {
        size_t remaining = (size_t)info.st_size;
        int sent = http_send_head(client, 200, mime_type_for(name), remaining, NULL);
        unsigned char buffer[8192];
        while (sent == 0 && remaining > 0) {
            size_t size = remaining < sizeof buffer ? remaining : sizeof buffer;
            ssize_t bytes = read(fd, buffer, size);
            if (bytes < 0 && errno == EINTR)
                continue;
            if (bytes <= 0)
                break;
            sent = stream_send(client, buffer, (size_t)bytes);
            remaining -= (size_t)bytes;
        }
    }
    close(fd);
}

static void handle_echo(Stream *client, const Request *request)
{
    if (http_send_head(client, 200, "application/octet-stream", request->head.length, NULL) == 0)
        stream_send(client, request->body, request->head.length);
}

static int send_backend_request(Stream *backend, const Route *route, const Request *request)
{
    char first_line[HTTP_TARGET_LIMIT + 128];
    int bytes = snprintf(first_line, sizeof first_line, "%s %s HTTP/1.1\r\nHost: %s:%u\r\n",
                         request->method, request->target, route->ip, (unsigned)route->port);
    if (bytes < 0 || (size_t)bytes >= sizeof first_line ||
        stream_send(backend, first_line, (size_t)bytes) == -1)
        return -1;

    for (size_t i = 0; i < request->head.count; i++) {
        Header header = request->head.headers[i];
        if (strcasecmp(header.name, "Host") != 0 &&
            http_forward_header(&request->head, header.name) &&
            http_send_line(backend, header.name, header.value) == -1)
            return -1;
    }

    char ending[160];
    bytes = snprintf(ending, sizeof ending, "Via: %s mini-webserver\r\n"
                     "Connection: close\r\nContent-Length: %zu\r\n\r\n",
                     request->version + 5, request->head.length);
    if (bytes < 0 || (size_t)bytes >= sizeof ending ||
        stream_send(backend, ending, (size_t)bytes) == -1)
        return -1;
    return stream_send(backend, request->body, request->head.length);
}

static int send_backend_head(Stream *client, const UpstreamResponse *response)
{
    char status[16];
    int bytes = snprintf(status, sizeof status, "HTTP/1.1 %d ", response->status);
    if (bytes < 0 || (size_t)bytes >= sizeof status ||
        stream_send(client, status, (size_t)bytes) == -1 ||
        stream_send(client, response->reason, strlen(response->reason)) == -1 ||
        stream_send(client, "\r\n", 2) == -1)
        return -1;

    for (size_t i = 0; i < response->head.count; i++) {
        Header header = response->head.headers[i];
        if (http_forward_header(&response->head, header.name) &&
            http_send_line(client, header.name, header.value) == -1)
            return -1;
    }

    if (response->head.has_length && response->status != 204) {
        char length[32];
        bytes = snprintf(length, sizeof length, "%zu", response->head.length);
        if (bytes < 0 || (size_t)bytes >= sizeof length ||
            http_send_line(client, "Content-Length", length) == -1)
            return -1;
    }
    return stream_send(client, "Connection: close\r\n\r\n",
                       sizeof "Connection: close\r\n\r\n" - 1);
}

static void relay_body(Stream *client, Stream *backend, const UpstreamResponse *response)
{
    unsigned char buffer[8192];
    size_t remaining = response->head.length;
    while (!response->head.has_length || remaining > 0) {
        size_t size = sizeof buffer;
        if (response->head.has_length && remaining < size)
            size = remaining;
        ssize_t bytes = http_read_body(backend, buffer, size);
        if (bytes <= 0 || stream_send(client, buffer, (size_t)bytes) == -1)
            return;
        if (response->head.has_length)
            remaining -= (size_t)bytes;
    }
}

static void handle_proxy(Stream *client, const Route *route, const Request *request)
{
    int64_t deadline = net_deadline();
    int fd = net_connect(route->ip, route->port, deadline);
    int error = 0;
    if (fd == -1) {
        error = errno == ETIMEDOUT ? 504 : 502;
    } else {
        Stream backend;
        stream_init(&backend, fd, deadline);
        UpstreamResponse response;
        if (send_backend_request(&backend, route, request) == -1) {
            error = errno == ETIMEDOUT ? 504 : 502;
        } else {
            do {
                error = http_read_response(&backend, &response);
                if (error == 0 && response.status == 101)
                    error = 502;
            } while (error == 0 && response.status < 200);

            if (error == 0 && response.head.has_transfer_encoding)
                error = 502;
            if (error == 0) {
                client->deadline = net_deadline();
                if (send_backend_head(client, &response) == 0 &&
                    response.status != 204 && response.status != 304)
                    relay_body(client, &backend, &response);
            }
        }
        close(fd);
    }

    if (error != 0) {
        client->deadline = net_deadline();
        http_send_error(client, error, NULL);
    }
}

void handle_client(int fd, const ServerConfig *config)
{
    Stream client;
    stream_init(&client, fd, net_deadline());
    Request request = {0};
    int status = http_read_request(&client, &request);
    client.deadline = net_deadline();

    if (status > 0) {
        if (http_send_error(&client, status, NULL) == 0)
            net_finish_error(fd);
    } else if (status == 0) {
        bool get = strcmp(request.method, "GET") == 0;
        bool post = strcmp(request.method, "POST") == 0;
        const Route *route = match_route(config, request.path);
        bool echo = strcmp(request.path, "/echo") == 0;
        if (echo && post)
            handle_echo(&client, &request);
        else if (echo)
            http_send_error(&client, 405, "POST");
        else if (route != NULL && (get || post))
            handle_proxy(&client, route, &request);
        else if (route != NULL)
            http_send_error(&client, 405, "GET, POST");
        else if (get)
            handle_static(&client, config->root_fd, &request);
        else
            http_send_error(&client, 405, "GET");
    }

    free(request.body);
    free(request.path);
}
