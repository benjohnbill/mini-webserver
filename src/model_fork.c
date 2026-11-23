#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cache.h"
#include "conn.h"
#include "model.h"

static void reap_children(int signo)
{
    (void)signo;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

int model_fork_run(int listen_fd, const struct config *config)
{
    struct sigaction action = {0};
    action.sa_handler = reap_children;
    sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD, &action, NULL);

    while (!stop_requested) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            close(listen_fd);
            conn_serve(conn_fd, config);
            close(conn_fd);
            /* A private cache is a copy that dies with the child. */
            if (config->cache != NULL &&
                config->cache_options.share == CACHE_SHARE_PRIVATE) {
                cache_report(config->cache);
            }
            _exit(0);
        }
        close(conn_fd);
    }
    return 0;
}
