/* CGI/1.1 (RFC 3875): fork + execve a script, feed it the request body on
 * stdin, collect stdout and turn the CGI header block into an HTTP reply.
 *
 * We fork from a multithreaded process, so the child may only call
 * async-signal-safe functions before execve(). Everything the child needs
 * (argv, envp, signal sets) is therefore built before fork(). */
#ifdef __linux__
#define _GNU_SOURCE                   /* pipe2() */
#endif
#include "cgi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CGI_MAX_ENV 96
#define CGI_ENV_LEN (HTTP_MAX_URI + 128)

typedef struct {
    char *vars[CGI_MAX_ENV + 1];      /* NULL-terminated envp */
    char store[CGI_MAX_ENV][CGI_ENV_LEN];
    size_t n;
} cgi_env;

/* Append "name=value"; silently skips variables that do not fit, which only
 * happens for absurdly long client headers. */
static void env_add(cgi_env *e, const char *name, const char *value)
{
    if (e->n >= CGI_MAX_ENV)
        return;
    int w = snprintf(e->store[e->n], CGI_ENV_LEN, "%s=%s", name, value);
    if (w < 0 || w >= CGI_ENV_LEN)
        return;
    e->vars[e->n] = e->store[e->n];
    e->n++;
    e->vars[e->n] = NULL;
}

static void build_env(cgi_env *e, const http_request *req,
                      const server_config *cfg, const char *client_ip,
                      const char *script_name, const char *script_file,
                      const char *path_info)
{
    char num[32];
    e->n = 0;
    e->vars[0] = NULL;

    env_add(e, "GATEWAY_INTERFACE", "CGI/1.1");
    env_add(e, "SERVER_SOFTWARE", "mini-webserver");
    env_add(e, "SERVER_PROTOCOL", req->version_minor ? "HTTP/1.1" : "HTTP/1.0");
    env_add(e, "REQUEST_METHOD", req->method);
    env_add(e, "REQUEST_URI", req->uri);
    env_add(e, "QUERY_STRING", req->query);
    env_add(e, "SCRIPT_NAME", script_name);
    env_add(e, "SCRIPT_FILENAME", script_file);
    env_add(e, "PATH_INFO", path_info);
    env_add(e, "REMOTE_ADDR", client_ip ? client_ip : "");
    env_add(e, "PATH", "/usr/local/bin:/usr/bin:/bin");
    snprintf(num, sizeof num, "%d", cfg->port);
    env_add(e, "SERVER_PORT", num);

    /* SERVER_NAME: the Host header without its port. */
    char host[256] = "localhost";
    const char *h = http_get_header(req, "Host");
    if (h != NULL && *h != '\0') {
        size_t hl = strcspn(h, ":");
        if (hl > 0 && hl < sizeof host) {
            memcpy(host, h, hl);
            host[hl] = '\0';
        }
    }
    env_add(e, "SERVER_NAME", host);

    if (req->body_len > 0 || req->has_content_length) {
        snprintf(num, sizeof num, "%zu", req->body_len);
        env_add(e, "CONTENT_LENGTH", num);
    }
    const char *ct = http_get_header(req, "Content-Type");
    if (ct != NULL)
        env_add(e, "CONTENT_TYPE", ct);

    /* Every other request header becomes HTTP_<NAME> (RFC 3875 4.1.18). */
    for (size_t i = 0; i < req->header_count; i++) {
        const http_header *hd = &req->headers[i];
        if (strcasecmp(hd->name, "Content-Type") == 0 ||
            strcasecmp(hd->name, "Content-Length") == 0)
            continue;
        char name[sizeof hd->name + 8] = "HTTP_";
        size_t o = 5;
        for (size_t j = 0; hd->name[j] && o + 1 < sizeof name; j++) {
            unsigned char ch = (unsigned char)hd->name[j];
            name[o++] = ch == '-' ? '_' : (char)toupper(ch);
        }
        name[o] = '\0';
        env_add(e, name, hd->value);
    }
}

/* ------------------------------------------------------------------ */

static int map_errno(int err)
{
    if (err == ENOENT || err == ENOTDIR)
        return 404;
    if (err == EACCES)
        return 403;
    return 500;
}

/* Resolve "/cgi-bin/<name>[/info]" to a real executable inside cgi_dir. */
static int resolve_script(const char *path, const char *cgi_dir,
                          char *name, size_t name_size,
                          char *resolved, const char **path_info)
{
    const char *rest = path + strlen("/cgi-bin/");
    size_t nlen = strcspn(rest, "/");
    if (nlen == 0 || nlen >= name_size)
        return 404;
    memcpy(name, rest, nlen);
    name[nlen] = '\0';
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 403;
    *path_info = rest + nlen;

    char candidate[PATH_MAX];
    int w = snprintf(candidate, sizeof candidate, "%s/%s", cgi_dir, name);
    if (w < 0 || (size_t)w >= sizeof candidate)
        return 404;
    if (realpath(candidate, resolved) == NULL)
        return map_errno(errno);

    /* A symlink in cgi-bin must not lead outside of it. */
    size_t dl = strlen(cgi_dir);
    if (strncmp(resolved, cgi_dir, dl) != 0 || resolved[dl] != '/')
        return 403;

    struct stat st;
    if (stat(resolved, &st) != 0)
        return map_errno(errno);
    if (!S_ISREG(st.st_mode) || access(resolved, X_OK) != 0)
        return 403;
    return 0;
}

/* pipe() whose fds are close-on-exec, so CGI children forked concurrently
 * by other worker threads do not inherit them (an inherited write end
 * would keep our read end from ever seeing EOF). */
static int make_pipe(int p[2])
{
#ifdef __linux__
    return pipe2(p, O_CLOEXEC);
#else
    if (pipe(p) != 0)
        return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);  /* small race on non-Linux */
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static long ms_since(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - t0->tv_sec) * 1000 +
           (now.tv_nsec - t0->tv_nsec) / 1000000;
}

/* The script runs in its own process group, so this also kills anything
 * it spawned. Fall back to the pid alone if setpgid had not happened yet. */
static void kill_script(pid_t pid)
{
    if (kill(-pid, SIGKILL) != 0)
        kill(pid, SIGKILL);
}

/* Wait for the child, but no longer than the overall CGI deadline: a
 * script can close stdout and keep running. Returns 0 if it exited on its
 * own, 1 if it had to be killed. */
static int reap(pid_t pid, const struct timespec *t0, int *status)
{
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR))
            return 0;
        if (r == 0 && ms_since(t0) >= CGI_TIMEOUT_MS)
            break;
        if (r == 0) {
            struct timespec ts = { 0, 10 * 1000 * 1000 }; /* 10 ms */
            nanosleep(&ts, NULL);
        }
    }
    kill_script(pid);
    while (waitpid(pid, status, 0) < 0 && errno == EINTR)
        ;
    return 1;
}

/* Fork, exec and exchange data. Returns 0 with *out / *out_len set, or an
 * error status. Uses poll() on both pipes so a script that writes a lot
 * before reading its stdin cannot deadlock us. */
static int execute(const char *script, const char *dir, char *const envp[],
                   const char *body, size_t body_len,
                   char **out, size_t *out_len)
{
    int in_p[2] = { -1, -1 }, out_p[2] = { -1, -1 };
    if (make_pipe(out_p) != 0)
        return 500;
    if (body_len > 0 && make_pipe(in_p) != 0) {
        close_fd(&out_p[0]);
        close_fd(&out_p[1]);
        return 500;
    }

    /* Prepared before fork(): the child must not allocate or lock. */
    char *argv[] = { (char *)script, NULL };
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigset_t none;
    sigemptyset(&none);

    pid_t pid = fork();
    if (pid < 0) {
        close_fd(&out_p[0]); close_fd(&out_p[1]);
        close_fd(&in_p[0]); close_fd(&in_p[1]);
        return 500;
    }
    if (pid == 0) {
        /* child: only async-signal-safe calls from here on */
        setpgid(0, 0);                  /* own group: kill(-pid) reaches all */
        int in_fd = in_p[0] >= 0 ? in_p[0] : open("/dev/null", O_RDONLY);
        if (in_fd < 0 || dup2(in_fd, STDIN_FILENO) < 0 ||
            dup2(out_p[1], STDOUT_FILENO) < 0)
            _exit(127);
        /* The pipe ends are close-on-exec; /dev/null is not. */
        if (in_p[0] < 0 && in_fd != STDIN_FILENO)
            close(in_fd);
        if (chdir(dir) != 0)
            _exit(127);
        /* The server ignores SIGPIPE and blocks SIGINT/SIGTERM in workers;
         * both would otherwise survive execve() into the script. */
        sigaction(SIGPIPE, &dfl, NULL);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execve(script, argv, envp);
        _exit(127);
    }

    /* parent; setpgid here too closes the race with the child's own call */
    setpgid(pid, pid);
    close_fd(&out_p[1]);
    close_fd(&in_p[0]);
    int rfd = out_p[0], wfd = in_p[1];
    set_nonblock(rfd);
    if (wfd >= 0)
        set_nonblock(wfd);

    size_t cap = 16384, len = 0, sent = 0;
    char *buf = malloc(cap);
    int result = buf ? 0 : 500;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (result == 0 && rfd >= 0) {
        long left = CGI_TIMEOUT_MS - ms_since(&t0);
        if (left <= 0) {
            result = 504;
            break;
        }
        struct pollfd pf[2];
        nfds_t nf = 0;
        pf[nf].fd = rfd; pf[nf].events = POLLIN; pf[nf].revents = 0; nf++;
        if (wfd >= 0) {
            pf[nf].fd = wfd; pf[nf].events = POLLOUT; pf[nf].revents = 0; nf++;
        }
        int pr = poll(pf, nf, (int)left);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            result = 500;
            break;
        }
        if (pr == 0)
            continue;                   /* re-check the deadline */

        if (nf == 2 && pf[1].revents) {
            ssize_t w = write(wfd, body + sent, body_len - sent);
            if (w > 0)
                sent += (size_t)w;
            /* EPIPE: script exited without reading its input; fine. */
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || sent == body_len)
                close_fd(&wfd);         /* EOF on the script's stdin */
        }
        if (pf[0].revents) {
            if (len == cap) {
                if (cap >= CGI_MAX_OUTPUT) {
                    result = 500;       /* output too large */
                    break;
                }
                char *nb = realloc(buf, cap * 2);
                if (nb == NULL) {
                    result = 500;
                    break;
                }
                buf = nb;
                cap *= 2;
            }
            ssize_t r = read(rfd, buf + len, cap - len);
            if (r > 0)
                len += (size_t)r;
            else if (r == 0)
                close_fd(&rfd);         /* script closed stdout: done */
            else if (errno != EAGAIN && errno != EINTR)
                result = 500;
        }
    }
    close_fd(&rfd);
    close_fd(&wfd);

    int wstatus = 0;
    if (result != 0)
        kill_script(pid);               /* timed out or misbehaved */
    if (reap(pid, &t0, &wstatus) && result == 0)
        result = 504;                   /* stdout closed, but never exited */

    if (result == 0 && len == 0)
        result = 500;                   /* exec failed or no output at all */
    if (result != 0) {
        free(buf);
        return result;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* CGI response header block                                           */
/* ------------------------------------------------------------------ */

static int append(char *dst, size_t size, const char *name, const char *value)
{
    size_t used = strlen(dst);
    int w = snprintf(dst + used, size - used, "%s: %s\r\n", name, value);
    return (w < 0 || (size_t)w >= size - used) ? -1 : 0;
}

/* The header block ends at the first empty line; scripts commonly use bare
 * LF, so both "\n\n" and "\r\n\r\n" are accepted. */
static int parse_output(cgi_result *res)
{
    char *p = res->output;
    char *end = p + res->body_len;  /* body_len holds total length here */
    int have_location = 0;

    res->status = 0;
    res->content_type[0] = '\0';
    res->headers[0] = '\0';

    for (;;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl == NULL)
            return 500;                 /* header block never terminated */
        char *line_end = (nl > p && nl[-1] == '\r') ? nl - 1 : nl;
        if (line_end == p) {            /* empty line: body follows */
            p = nl + 1;
            break;
        }
        *line_end = '\0';
        char *colon = strchr(p, ':');
        if (colon == NULL || colon == p)
            return 500;
        *colon = '\0';
        for (char *c = p; *c; c++)
            if (*c == ' ' || *c == '\t' || (unsigned char)*c < 0x21)
                return 500;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        for (char *c = value; *c; c++)
            if ((unsigned char)*c < 0x20 && *c != '\t')
                return 500;

        if (strcasecmp(p, "Status") == 0) {
            if (!isdigit((unsigned char)value[0]) ||
                !isdigit((unsigned char)value[1]) ||
                !isdigit((unsigned char)value[2]) ||
                (value[3] != '\0' && value[3] != ' '))
                return 500;
            res->status = atoi(value);
            /* 1xx are interim responses; a script cannot send one as its
             * final answer. */
            if (res->status < 200 || res->status > 599)
                return 500;
        } else if (strcasecmp(p, "Content-Type") == 0) {
            if (strlen(value) >= sizeof res->content_type)
                return 500;
            strcpy(res->content_type, value);
        } else if (strcasecmp(p, "Content-Length") == 0 ||
                   strcasecmp(p, "Connection") == 0 ||
                   strcasecmp(p, "Transfer-Encoding") == 0 ||
                   strcasecmp(p, "Keep-Alive") == 0 ||
                   strcasecmp(p, "Server") == 0 ||
                   strcasecmp(p, "Date") == 0) {
            /* framing / hop-by-hop headers are ours to generate */
        } else {
            if (strcasecmp(p, "Location") == 0)
                have_location = 1;
            if (append(res->headers, sizeof res->headers, p, value) < 0)
                return 500;
        }
        p = nl + 1;
    }

    if (res->status == 0)
        res->status = have_location ? 302 : 200;
    if (res->content_type[0] == '\0') {
        /* RFC 3875 6.2: required unless redirecting or bodyless */
        if (!have_location && res->status != 204 && res->status != 304)
            return 500;
        strcpy(res->content_type, "text/plain; charset=utf-8");
    }
    res->body = p;
    res->body_len = (size_t)(end - p);
    if (res->status == 204 || res->status == 304)
        res->body_len = 0;              /* these never carry a body */
    return 0;
}

int cgi_run(const http_request *req, const server_config *cfg,
            const char *client_ip, cgi_result *res)
{
    char name[256], resolved[PATH_MAX], script_name[300];
    const char *path_info = "";

    memset(res, 0, sizeof *res);
    int st = resolve_script(req->path, cfg->cgi_dir, name, sizeof name,
                            resolved, &path_info);
    if (st != 0)
        return st;
    snprintf(script_name, sizeof script_name, "/cgi-bin/%s", name);

    cgi_env *env = malloc(sizeof *env);
    if (env == NULL)
        return 500;
    build_env(env, req, cfg, client_ip, script_name, resolved, path_info);

    size_t out_len = 0;
    st = execute(resolved, cfg->cgi_dir, env->vars, req->body, req->body_len,
                 &res->output, &out_len);
    free(env);
    if (st != 0)
        return st;

    res->body_len = out_len;
    st = parse_output(res);
    if (st != 0) {
        cgi_result_free(res);
        return st;
    }
    return 0;
}

void cgi_result_free(cgi_result *res)
{
    free(res->output);
    res->output = NULL;
    res->body = NULL;
    res->body_len = 0;
}
