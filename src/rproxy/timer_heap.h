#ifndef TIMER_HEAP_H
#define TIMER_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decision module of the Reverse Proxy: the timers of one Event loop, in a
   binary min-heap ordered by deadline. The owner of a timer embeds a
   struct timer and finds itself from it. Time is an argument (seconds, any
   fixed origin), so tests need no clock. Not safe to share between
   threads: each Event loop has its own heap. */

/* index is TIMER_UNSET while the timer is in no heap. */
#define TIMER_UNSET SIZE_MAX

struct timer {
    double deadline;
    size_t index;
};

/* Sets timer to "in no heap". Call it before the first add. */
void timer_init(struct timer *timer);
bool timer_is_set(const struct timer *timer);

/* Returns NULL if capacity is 0 or memory cannot be set up. */
struct timer_heap *timer_heap_create(size_t capacity);
void timer_heap_destroy(struct timer_heap *timers);

/* Puts timer into the heap at deadline. If timer is already there, it
   moves to the new deadline. Returns false, and changes nothing, if the
   heap is full and timer is not in it. */
bool timer_heap_add(struct timer_heap *timers, struct timer *timer,
                    double deadline);

/* Takes timer out of the heap. Does nothing if it is not there. */
void timer_heap_remove(struct timer_heap *timers, struct timer *timer);

/* Returns the timer with the earliest deadline, or NULL if the heap is
   empty. It stays in the heap. */
struct timer *timer_heap_find_first(const struct timer_heap *timers);

/* Takes out and returns the timer with the earliest deadline if that
   deadline is at or before now. Returns NULL otherwise. */
struct timer *timer_heap_remove_expired(struct timer_heap *timers, double now);

#endif
