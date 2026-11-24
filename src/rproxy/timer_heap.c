#include "timer_heap.h"

/* Stub: the tests fail until the heap is written. */

void timer_init(struct timer *timer)
{
    (void)timer;
}

bool timer_is_set(const struct timer *timer)
{
    (void)timer;

    return false;
}

struct timer_heap *timer_heap_create(size_t capacity)
{
    (void)capacity;

    return NULL;
}

void timer_heap_destroy(struct timer_heap *timers)
{
    (void)timers;
}

bool timer_heap_add(struct timer_heap *timers, struct timer *timer,
                    double deadline)
{
    (void)timers;
    (void)timer;
    (void)deadline;

    return false;
}

void timer_heap_remove(struct timer_heap *timers, struct timer *timer)
{
    (void)timers;
    (void)timer;
}

struct timer *timer_heap_find_first(const struct timer_heap *timers)
{
    (void)timers;

    return NULL;
}

struct timer *timer_heap_remove_expired(struct timer_heap *timers, double now)
{
    (void)timers;
    (void)now;

    return NULL;
}
