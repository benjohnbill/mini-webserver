#ifndef SERVER_H
#define SERVER_H

#include <limits.h>
#include "route.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    int port;                     /* -p, default 8080 */
    char root[PATH_MAX];          /* -r, static root, default ./www (realpath'd) */
    char cgi_dir[PATH_MAX];       /* -c, default ./cgi-bin; served at /cgi-bin/ */
    int threads;                  /* -t, worker threads, default 8 */
    int queue_size;               /* -q, accept queue capacity, default 256 */
    int forward_proxy;            /* -F, accept absolute-URI requests */
    int proxy_timeout_ms;         /* -T, default 3000 */
    int keepalive_timeout_ms;     /* idle keep-alive timeout, default 5000 */
    char name[64];                /* -n, sent as X-Server-Name if set */
    route_table routes;           /* -R spec, repeatable */
} server_config;

/* Parse argv. Returns 0 ok, -1 on error (usage printed to stderr). */
int config_parse_args(server_config *cfg, int argc, char **argv);

/* MIME type for a file path by extension; "application/octet-stream"
 * when unknown. Case-insensitive. */
const char *mime_type_for_path(const char *path);

#endif
