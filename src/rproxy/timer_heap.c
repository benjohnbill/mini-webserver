#include "timer_heap.h"

#include <stdlib.h>

/* slots[0] has the earliest deadline. The children of slot i are slots
   2i + 1 and 2i + 2. Each timer keeps its slot in timer->index, so a
   remove or a move finds it with no search. */
struct timer_heap {
    struct timer **slots;
    size_t count;
    size_t capacity;
};

static void heap_place(struct timer_heap *timers, struct timer *timer,
                       size_t index);
static void heap_fix(struct timer_heap *timers, size_t index);
static void heap_sift_up(struct timer_heap *timers, size_t index);
static void heap_sift_down(struct timer_heap *timers, size_t index);
static size_t heap_find_earlier_child(const struct timer_heap *timers,
                                      size_t index);

void timer_init(struct timer *timer)
{
    timer->deadline = 0;
    timer->index = TIMER_UNSET;
}

bool timer_is_set(const struct timer *timer)
{
    return timer->index != TIMER_UNSET;
}

struct timer_heap *timer_heap_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }
    struct timer_heap *timers = malloc(sizeof(*timers));
    if (timers == NULL) {
        return NULL;
    }
    timers->slots = calloc(capacity, sizeof(*timers->slots));
    if (timers->slots == NULL) {
        free(timers);
        return NULL;
    }
    timers->count = 0;
    timers->capacity = capacity;

    return timers;
}

void timer_heap_destroy(struct timer_heap *timers)
{
    if (timers == NULL) {
        return;
    }
    free(timers->slots);
    free(timers);
}

bool timer_heap_add(struct timer_heap *timers, struct timer *timer,
                    double deadline)
{
    if (timer_is_set(timer)) {
        timer->deadline = deadline;
        heap_fix(timers, timer->index);
        return true;
    }
    if (timers->count == timers->capacity) {
        return false;
    }
    timer->deadline = deadline;
    heap_place(timers, timer, timers->count);
    timers->count++;
    heap_sift_up(timers, timer->index);

    return true;
}

/* The last timer fills the hole, then moves up or down to its place. */
void timer_heap_remove(struct timer_heap *timers, struct timer *timer)
{
    if (!timer_is_set(timer)) {
        return;
    }
    size_t hole = timer->index;
    timer->index = TIMER_UNSET;
    timers->count--;
    if (hole == timers->count) {
        return;
    }
    heap_place(timers, timers->slots[timers->count], hole);
    heap_fix(timers, hole);
}

struct timer *timer_heap_find_first(const struct timer_heap *timers)
{
    return timers->count > 0 ? timers->slots[0] : NULL;
}

struct timer *timer_heap_remove_expired(struct timer_heap *timers, double now)
{
    struct timer *first = timer_heap_find_first(timers);
    if (first == NULL || first->deadline > now) {
        return NULL;
    }
    timer_heap_remove(timers, first);

    return first;
}

static void heap_place(struct timer_heap *timers, struct timer *timer,
                       size_t index)
{
    timers->slots[index] = timer;
    timer->index = index;
}

/* A changed deadline can break the order with the parent or with a
   child, but not with both. */
static void heap_fix(struct timer_heap *timers, size_t index)
{
    struct timer *timer = timers->slots[index];
    heap_sift_up(timers, index);
    heap_sift_down(timers, timer->index);
}

static void heap_sift_up(struct timer_heap *timers, size_t index)
{
    struct timer *timer = timers->slots[index];
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (timers->slots[parent]->deadline <= timer->deadline) {
            break;
        }
        heap_place(timers, timers->slots[parent], index);
        index = parent;
    }
    heap_place(timers, timer, index);
}

static void heap_sift_down(struct timer_heap *timers, size_t index)
{
    struct timer *timer = timers->slots[index];
    for (;;) {
        size_t child = heap_find_earlier_child(timers, index);
        if (child == TIMER_UNSET ||
            timers->slots[child]->deadline >= timer->deadline) {
            break;
        }
        heap_place(timers, timers->slots[child], index);
        index = child;
    }
    heap_place(timers, timer, index);
}

/* Returns TIMER_UNSET if the slot has no child. */
static size_t heap_find_earlier_child(const struct timer_heap *timers,
                                      size_t index)
{
    size_t left = 2 * index + 1;
    if (left >= timers->count) {
        return TIMER_UNSET;
    }
    size_t right = left + 1;
    if (right < timers->count &&
        timers->slots[right]->deadline < timers->slots[left]->deadline) {
        return right;
    }

    return left;
}
