#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

/* I/O layer of the Reverse Proxy: one Event loop. It has its own epoll
   instance, its own listen socket with SO_REUSEPORT, and its own timers.
   One thread runs one loop: the loop is not safe to share. The decision
   tables in struct rproxy are shared, and their mutexes protect them. */

#include <stdbool.h>

struct rproxy;
struct event_loop;

/* Asks if no socket listens on port. A plain bind (no SO_REUSEPORT) fails
   while another socket holds the port, also one with SO_REUSEPORT. So a
   second rproxy cannot join the listen sockets of a first one. */
bool event_loop_is_port_free(int port);

/* Opens the listen socket on rproxy->config.port. Returns NULL, and
   prints the cause, if a part cannot be set up. */
struct event_loop *event_loop_create(struct rproxy *rproxy);
void event_loop_destroy(struct event_loop *loop);

/* Serves connections until rproxy->is_stopping is set, then closes the
   open connections. Returns 0, or -1 if epoll_wait failed. */
int event_loop_run(struct event_loop *loop);

#endif
