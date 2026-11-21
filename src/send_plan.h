#ifndef SEND_PLAN_H
#define SEND_PLAN_H

#include <stddef.h>

#include "response.h"

#define SEND_DONE 0
#define SEND_AGAIN 1
#define SEND_ERROR (-1)

/* I/O layer. Writes the header, then the body, from *offset. Returns
   SEND_DONE, SEND_AGAIN (the socket would block), or SEND_ERROR. */
int send_plan_some(const struct response_plan *plan, int conn_fd,
                   size_t *offset);

#endif
