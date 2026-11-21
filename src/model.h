#ifndef MODEL_H
#define MODEL_H

#include <signal.h>

#include "config.h"

/* Set by the SIGINT and SIGTERM handler in main.c. Each loop stops on it. */
extern volatile sig_atomic_t stop_requested;

/* Each returns 0 at a clean shutdown. The model that accepted a conn_fd
   closes it. */
int model_iter_run(int listen_fd, const struct config *config);
int model_fork_run(int listen_fd, const struct config *config);
int model_pool_run(int listen_fd, const struct config *config);
int model_epoll_run(int listen_fd, const struct config *config);

#endif
