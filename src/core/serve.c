#include "serve.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mime.h"
#include "router.h"

static int static_file_open(const struct request *request,
                            const struct config *config,
                            struct response_plan *plan);
static int path_resolve(char *path, size_t size, struct stat *info);
static int errno_to_status(int error);
static int path_resolve_index(char *path, size_t size, struct stat *info);

void serve_request(const struct request *request, const struct config *config,
                   struct response_plan *plan)
{
    struct route route;
    int status = router_resolve(request, &route);
    if (status == 405) {
        response_plan_init_not_allowed(plan, route.allowed_method);
        return;
    }

    if (status == 0 && route.kind == ROUTE_PROXY) {
        /* The Shared core has no sockets. conn.c fetches a Proxy Route
           before it calls serve_request. Only epoll gets here. */
        status = 501;
    } else if (status == 0 && route.kind == ROUTE_HANDLER) {
        status = route.handler(request, plan);
    } else if (status == 0) {
        status = static_file_open(request, config, plan);
    }
    if (status != 0) {
        response_plan_init_error(plan, status,
                                 !serve_keeps_open(request, status));
    }
}

bool serve_keeps_open(const struct request *request, int status)
{
    return status < 400 && !request->wants_close;
}

static int static_file_open(const struct request *request,
                            const struct config *config,
                            struct response_plan *plan)
{
    char path[PATH_BUFFER_SIZE];
    struct stat info;
    int length =
        snprintf(path, sizeof(path), "%s%s", config->root, request->target);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return 500;
    }
    int status = path_resolve(path, sizeof(path), &info);
    if (status != 0) {
        return status;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno_to_status(errno);
    }
    if (!response_plan_init(plan, 200, mime_find_type(path), info.st_size,
                            !serve_keeps_open(request, 200))) {
        close(fd);
        return 0;
    }
    plan->file_fd = fd;
    plan->file_size = info.st_size;

    return 0;
}

/* A folder becomes its index.html. Only a regular file passes. */
static int path_resolve(char *path, size_t size, struct stat *info)
{
    if (stat(path, info) < 0) {
        return errno_to_status(errno);
    }
    if (S_ISDIR(info->st_mode)) {
        int status = path_resolve_index(path, size, info);
        if (status != 0) {
            return status;
        }
    }
    if (!S_ISREG(info->st_mode)) {
        return 403;
    }

    return 0;
}

static int errno_to_status(int error)
{
    if (error == ENOENT || error == ENOTDIR) {
        return 404;
    }
    if (error == EACCES) {
        return 403;
    }

    return 500;
}

/* A folder becomes its index.html. */
static int path_resolve_index(char *path, size_t size, struct stat *info)
{
    size_t len = strlen(path);
    const char *index_name =
        path[len - 1] == '/' ? "index.html" : "/index.html";
    if (len + strlen(index_name) >= size) {
        return 500;
    }
    strcat(path, index_name);
    if (stat(path, info) < 0) {
        return errno_to_status(errno);
    }

    return 0;
}
