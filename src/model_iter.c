#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>

#include "conn.h"
#include "model.h"

int model_iter_run(int listen_fd, const struct config *config)
{
    while (!stop_requested) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        conn_serve(conn_fd, config);
        close(conn_fd);
    }
    return 0;
}
