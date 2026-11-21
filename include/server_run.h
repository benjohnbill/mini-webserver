#ifndef SERVER_RUN_H
#define SERVER_RUN_H

#include "server.h"

/* Internal: bind, run the accept loop + thread pool until SIGINT/SIGTERM,
 * then shut down cleanly. Returns 0 on clean shutdown, -1 on setup error. */
int server_run(server_config *cfg);

/* 1 if a decoded path contains a "." or ".." segment ("/a/../b", "/./x",
 * "/a/.."), else 0. Such requests are rejected with 400 before dispatch. */
int path_has_dot_segment(const char *path);

#endif
