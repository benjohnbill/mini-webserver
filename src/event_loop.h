#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "handlers.h"
#include <signal.h>

#if defined(__linux__)
#define NATIVE_EVENT_MODEL "epoll"
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define NATIVE_EVENT_MODEL "kqueue"
#endif

enum EventModel { USE_SELECT, USE_POLL, USE_NATIVE };
int run_event_loop(int listener, const ServerConfig *config, enum EventModel model,
                   const volatile sig_atomic_t *stopping);

#endif
