#include "event_loop.h"
#include "connection.h"

#include <errno.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(NATIVE_EVENT_MODEL)
#include <sys/event.h>
#endif

/* Slot zero is the listener. Each other slot watches whichever socket the
   HTTP transaction currently needs, including its backend socket. */
enum { SLOTS = 65 };
typedef struct {
    enum EventModel model;
    int queue;
    struct pollfd watch[SLOTS];
    Connection *clients[SLOTS];
    int client_fds[SLOTS];
} Loop;

static int set_interest(Loop *loop, int slot, struct pollfd next)
{
    struct pollfd old = loop->watch[slot];
    if (old.fd == next.fd && old.events == next.events)
        return 0;
    if (loop->model == USE_SELECT && next.fd >= FD_SETSIZE) {
        errno = ERANGE;
        return -1;
    }
    if (loop->model == USE_NATIVE) {
#if defined(__linux__)
        if (old.fd >= 0 && old.fd != next.fd &&
            epoll_ctl(loop->queue, EPOLL_CTL_DEL, old.fd, NULL) < 0)
            return -1;
        if (next.fd >= 0) {
            struct epoll_event ev = {.events = next.events == POLLIN ? EPOLLIN : EPOLLOUT};
            ev.data.u32 = (unsigned)slot;
            int op = old.fd == next.fd ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
            if (epoll_ctl(loop->queue, op, next.fd, &ev) < 0)
                return -1;
        }
#elif defined(NATIVE_EVENT_MODEL)
        struct kevent changes[2];
        int n = 0;
        if (old.fd >= 0) {
            EV_SET(&changes[n], old.fd, old.events == POLLIN ? EVFILT_READ : EVFILT_WRITE,
                   EV_DELETE, 0, 0, NULL);
            n++;
        }
        if (next.fd >= 0) {
            EV_SET(&changes[n], next.fd, next.events == POLLIN ? EVFILT_READ : EVFILT_WRITE,
                   EV_ADD, 0, 0, (void *)(uintptr_t)slot);
            n++;
        }
        if (kevent(loop->queue, changes, n, NULL, 0, NULL) < 0)
            return -1;
#endif
    }
    loop->watch[slot] = next;
    return 0;
}

static int wait_select(Loop *loop, int timeout, int *ready)
{
    fd_set readers, writers;
    FD_ZERO(&readers);
    FD_ZERO(&writers);
    int maxfd = -1;
    for (int i = 0; i < SLOTS; i++) {
        struct pollfd p = loop->watch[i];
        if (p.fd < 0)
            continue;
        FD_SET(p.fd, p.events == POLLIN ? &readers : &writers);
        if (p.fd > maxfd)
            maxfd = p.fd;
    }
    struct timeval tv = {.tv_sec = timeout / 1000, .tv_usec = timeout % 1000 * 1000};
    int count = select(maxfd + 1, &readers, &writers, NULL, &tv);
    if (count <= 0)
        return count;
    int n = 0;
    for (int i = 0; i < SLOTS; i++) {
        int fd = loop->watch[i].fd;
        if (fd >= 0 && (FD_ISSET(fd, &readers) || FD_ISSET(fd, &writers)))
            ready[n++] = i;
    }
    return n;
}

static int wait_ready(Loop *loop, int timeout, int *ready)
{
    if (loop->model == USE_SELECT)
        return wait_select(loop, timeout, ready);
    if (loop->model == USE_POLL) {
        int count = poll(loop->watch, SLOTS, timeout);
        if (count <= 0)
            return count;
        int n = 0;
        for (int i = 0; i < SLOTS; i++)
            if (loop->watch[i].revents)
                ready[n++] = i;
        return n;
    }
#if defined(__linux__)
    struct epoll_event events[SLOTS];
    int count = epoll_wait(loop->queue, events, SLOTS, timeout);
    for (int i = 0; i < count; i++)
        ready[i] = (int)events[i].data.u32;
    return count;
#elif defined(NATIVE_EVENT_MODEL)
    struct kevent events[SLOTS];
    struct timespec ts = {.tv_sec = timeout / 1000, .tv_nsec = timeout % 1000 * 1000000L};
    int count = kevent(loop->queue, NULL, 0, events, SLOTS, &ts);
    for (int i = 0; i < count; i++)
        ready[i] = (int)(uintptr_t)events[i].udata;
    return count;
#else
    errno = ENOSYS;
    return -1;
#endif
}

static void drop_client(Loop *loop, int slot)
{
    /* Closing both descriptors also removes any residual kernel registration. */
    set_interest(loop, slot, (struct pollfd){.fd = -1});
    loop->watch[slot].fd = -1;
    connection_free(loop->clients[slot]);
    loop->clients[slot] = NULL;
    close(loop->client_fds[slot]);
}

static int accept_one(Loop *loop, int listener, const ServerConfig *config)
{
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED ||
               errno == EINTR ? 0 : -1;
    int slot = 1;
    while (slot < SLOTS && loop->clients[slot] != NULL)
        slot++;
    if (slot == SLOTS || net_nonblocking(fd) < 0) {
        close(fd);
        return 0;
    }
    Connection *c = connection_new(fd, config);
    if (c == NULL) {
        close(fd);
        return 0;
    }
    loop->clients[slot] = c;
    loop->client_fds[slot] = fd;
    if (set_interest(loop, slot, connection_interest(c)) < 0)
        drop_client(loop, slot);
    return 0;
}

int run_event_loop(int listener, const ServerConfig *config, enum EventModel model,
                   const volatile sig_atomic_t *stopping)
{
    Loop loop = {.model = model, .queue = -1};
    for (int i = 0; i < SLOTS; i++)
        loop.watch[i].fd = -1;
    if (model == USE_NATIVE) {
#if defined(__linux__)
        loop.queue = epoll_create1(EPOLL_CLOEXEC);
#elif defined(NATIVE_EVENT_MODEL)
        loop.queue = kqueue();
#endif
        if (loop.queue < 0)
            return -1;
    }
    int result = set_interest(&loop, 0, (struct pollfd){.fd = listener, .events = POLLIN});
    while (result == 0) {
        if (*stopping && loop.watch[0].fd >= 0 &&
            set_interest(&loop, 0, (struct pollfd){.fd = -1}) < 0) {
            result = -1;
            break;
        }
        bool active = false;
        int timeout = 200;
        int64_t now = net_now_ms();
        for (int i = 1; i < SLOTS; i++) {
            Connection *c = loop.clients[i];
            if (c == NULL)
                continue;
            active = true;
            int64_t delay = connection_deadline(c) - now;
            if (delay <= 0) {
                connection_timeout(c);
                struct pollfd p = connection_interest(c);
                if (p.fd < 0 || set_interest(&loop, i, p) < 0) {
                    drop_client(&loop, i);
                    continue;
                }
                delay = connection_deadline(c) - now;
            }
            if (delay < timeout)
                timeout = (int)delay;
        }
        if (*stopping && !active)
            break;
        int ready[SLOTS];
        int n = wait_ready(&loop, timeout, ready);
        if (n < 0 && errno != EINTR)
            result = -1;
        for (int j = 0; j < n; j++) {
            int slot = ready[j];
            if (slot == 0) {
                if (*stopping)
                    continue;
                result = accept_one(&loop, listener, config);
                if (result < 0)
                    break;
            } else {
                Connection *c = loop.clients[slot];
                if (net_now_ms() >= connection_deadline(c))
                    connection_timeout(c);
                else
                    connection_step(c);
                struct pollfd p = connection_interest(c);
                if (p.fd < 0 || set_interest(&loop, slot, p) < 0)
                    drop_client(&loop, slot);
            }
        }
    }
    for (int i = 1; i < SLOTS; i++)
        if (loop.clients[i] != NULL)
            drop_client(&loop, i);
    if (loop.queue >= 0)
        close(loop.queue);
    return result;
}
