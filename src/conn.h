#ifndef CONN_H
#define CONN_H

#include "config.h"
#include "http_parse.h"
#include "response.h"

/* Blocking: reads to the empty line, then the body, in a keep-alive loop.
   Does not close conn_fd. */
void conn_serve(int conn_fd, const struct config *config);

/* --work: burns MS milliseconds of CPU time of this thread. The Handler
   time of a real Origin, for the demo. 0 does nothing. */
void conn_work_burn(int work_ms);

/* One line for --log. request is NULL if the request did not parse. */
void conn_log(const struct request *request, const struct response_plan *plan);

#endif
