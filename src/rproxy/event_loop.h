#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

/* I/O layer of the Reverse Proxy: one Event loop. It has its own epoll
   instance, its own listen socket with SO_REUSEPORT, and its own timers.
   One thread runs one loop: the loop is not safe to share. The decision
   tables in struct rproxy are shared, and their mutexes protect them. */

struct rproxy;
struct event_loop;

/* Opens the listen socket on rproxy->config.port. Returns NULL, and
   prints the cause, if a part cannot be set up. */
struct event_loop *event_loop_create(struct rproxy *rproxy);
void event_loop_destroy(struct event_loop *loop);

/* Serves connections until rproxy->is_stopping is set, then closes the
   open connections. Returns 0, or -1 if epoll_wait failed. */
int event_loop_run(struct event_loop *loop);

#endif
