#ifndef HEALTH_H
#define HEALTH_H

#include <stddef.h>
#include "server.h"

/* Active health checker: one thread that TCP-connects to every backend of
 * every route once a second and feeds the result to backend_report().
 * health_start returns 0 or -1; health_stop signals and joins (fast). */
int health_start(server_config *cfg);
void health_stop(void);

/* Render the /_lb/status text table into out. Returns its length, or -1
 * if it did not fit. */
int lb_status_render(char *out, size_t size, const server_config *cfg,
                     int busy, size_t queued,
                     unsigned long shed_queue, unsigned long shed_stale);

#endif
