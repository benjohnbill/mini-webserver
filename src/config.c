/* Command-line configuration. */
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "usage: %s [options]\n"
        "  -p PORT     listen port (default 8080)\n"
        "  -r DIR      static file root (default ./www)\n"
        "  -c DIR      CGI directory served at /cgi-bin/ (default ./cgi-bin)\n"
        "  -t N        worker threads (default 8, 1..256)\n"
        "  -q N        accept queue capacity (default 256, 1..65536)\n"
        "  -F          enable forward proxy (absolute-URI requests)\n"
        "  -T MS       proxy connect/IO timeout in ms (default 3000)\n"
        "  -k MS       keep-alive idle timeout in ms (default 5000)\n"
        "  -n NAME     send 'X-Server-Name: NAME' on responses\n"
        "  -R SPEC     reverse-proxy route, repeatable:\n"
        "              /api/=127.0.0.1:8001,127.0.0.1:8003\n"
        "  -h          show this help\n",
        prog);
}

/* Strict integer parse: whole string must be a number within [lo, hi]. */
static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int config_parse_args(server_config *cfg, int argc, char **argv)
{
    const char *prog = argc > 0 ? argv[0] : "webserver";
    const char *root = "./www";
    const char *cgi = "./cgi-bin";
    int opt;

    memset(cfg, 0, sizeof *cfg);
    cfg->port = 8080;
    cfg->threads = 8;
    cfg->queue_size = 256;
    cfg->proxy_timeout_ms = 3000;
    cfg->keepalive_timeout_ms = 5000;
    route_table_init(&cfg->routes);

    optind = 1;                         /* allow repeated calls (tests) */
    while ((opt = getopt(argc, argv, "p:r:c:t:q:FT:k:n:R:h")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_int(optarg, 1, 65535, &cfg->port) < 0) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return -1;
            }
            break;
        case 'r': root = optarg; break;
        case 'c': cgi = optarg; break;
        case 't':
            if (parse_int(optarg, 1, 256, &cfg->threads) < 0) {
                fprintf(stderr, "invalid thread count: %s\n", optarg);
                return -1;
            }
            break;
        case 'q':
            if (parse_int(optarg, 1, 65536, &cfg->queue_size) < 0) {
                fprintf(stderr, "invalid queue size: %s\n", optarg);
                return -1;
            }
            break;
        case 'F': cfg->forward_proxy = 1; break;
        case 'T':
            if (parse_int(optarg, 1, 600000, &cfg->proxy_timeout_ms) < 0) {
                fprintf(stderr, "invalid proxy timeout: %s\n", optarg);
                return -1;
            }
            break;
        case 'k':
            if (parse_int(optarg, 1, 600000, &cfg->keepalive_timeout_ms) < 0) {
                fprintf(stderr, "invalid keep-alive timeout: %s\n", optarg);
                return -1;
            }
            break;
        case 'n':
            /* The name goes into a response header: forbid CR/LF so it
             * cannot inject extra header lines. */
            if (strlen(optarg) >= sizeof cfg->name || strpbrk(optarg, "\r\n")) {
                fprintf(stderr, "invalid server name: %s\n", optarg);
                return -1;
            }
            strcpy(cfg->name, optarg);
            break;
        case 'R':
            if (route_table_add(&cfg->routes, optarg) != 0) {
                fprintf(stderr, "invalid route spec: %s\n", optarg);
                return -1;
            }
            break;
        case 'h':
            usage(stdout, prog);
            exit(EXIT_SUCCESS);
        default:
            usage(stderr, prog);
            return -1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        usage(stderr, prog);
        return -1;
    }

    /* Canonical absolute paths make the later "is this file inside the
     * root?" prefix check meaningful. */
    if (!is_dir(root) || realpath(root, cfg->root) == NULL) {
        fprintf(stderr, "static root is not a directory: %s\n", root);
        return -1;
    }
    if (!is_dir(cgi) || realpath(cgi, cfg->cgi_dir) == NULL) {
        fprintf(stderr, "CGI directory %s not found; CGI disabled\n", cgi);
        cfg->cgi_dir[0] = '\0';
    }
    return 0;
}
