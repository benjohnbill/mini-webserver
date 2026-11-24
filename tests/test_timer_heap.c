#include <stdio.h>

#include "check.h"
#include "timer_heap.h"

#define MANY 100

static struct timer_heap *timers_make(size_t capacity)
{
    struct timer_heap *timers = timer_heap_create(capacity);
    CHECK(timers != NULL);

    return timers;
}

static void an_empty_heap_has_no_first_timer(void)
{
    struct timer_heap *timers = timers_make(4);
    CHECK(timer_heap_find_first(timers) == NULL);
    CHECK(timer_heap_remove_expired(timers, 1e9) == NULL);
    timer_heap_destroy(timers);
}

static void a_new_timer_is_in_no_heap(void)
{
    struct timer timer;
    timer_init(&timer);
    CHECK(!timer_is_set(&timer));
}

static void the_first_timer_has_the_earliest_deadline(void)
{
    struct timer_heap *timers = timers_make(4);
    struct timer late;
    struct timer early;
    struct timer middle;
    timer_init(&late);
    timer_init(&early);
    timer_init(&middle);
    CHECK(timer_heap_add(timers, &late, 30));
    CHECK(timer_heap_add(timers, &early, 10));
    CHECK(timer_heap_add(timers, &middle, 20));
    CHECK(timer_is_set(&early));
    CHECK(timer_heap_find_first(timers) == &early);
    CHECK(timer_heap_find_first(timers) == &early);
    timer_heap_destroy(timers);
}

static void adding_a_timer_again_moves_it(void)
{
    struct timer_heap *timers = timers_make(4);
    struct timer moved;
    struct timer other;
    timer_init(&moved);
    timer_init(&other);
    CHECK(timer_heap_add(timers, &moved, 5));
    CHECK(timer_heap_add(timers, &other, 3));
    CHECK(timer_heap_add(timers, &moved, 1));
    CHECK(timer_heap_find_first(timers) == &moved);
    CHECK(timer_heap_add(timers, &moved, 9));
    CHECK(timer_heap_find_first(timers) == &other);
    CHECK(timer_heap_remove_expired(timers, 3) == &other);
    CHECK(timer_heap_remove_expired(timers, 3) == NULL);
    CHECK(timer_heap_remove_expired(timers, 9) == &moved);
    timer_heap_destroy(timers);
}

static void a_timer_expires_at_its_deadline_not_before(void)
{
    struct timer_heap *timers = timers_make(4);
    struct timer timer;
    timer_init(&timer);
    CHECK(timer_heap_add(timers, &timer, 10));
    CHECK(timer_heap_remove_expired(timers, 9.999) == NULL);
    CHECK(timer_heap_remove_expired(timers, 10) == &timer);
    CHECK(!timer_is_set(&timer));
    CHECK(timer_heap_find_first(timers) == NULL);
    timer_heap_destroy(timers);
}

static void expired_timers_come_out_in_deadline_order(void)
{
    struct timer_heap *timers = timers_make(8);
    struct timer at3;
    struct timer at1;
    struct timer at2;
    struct timer at10;
    timer_init(&at3);
    timer_init(&at1);
    timer_init(&at2);
    timer_init(&at10);
    CHECK(timer_heap_add(timers, &at3, 3));
    CHECK(timer_heap_add(timers, &at1, 1));
    CHECK(timer_heap_add(timers, &at10, 10));
    CHECK(timer_heap_add(timers, &at2, 2));
    CHECK(timer_heap_remove_expired(timers, 3) == &at1);
    CHECK(timer_heap_remove_expired(timers, 3) == &at2);
    CHECK(timer_heap_remove_expired(timers, 3) == &at3);
    CHECK(timer_heap_remove_expired(timers, 3) == NULL);
    CHECK(timer_heap_find_first(timers) == &at10);
    timer_heap_destroy(timers);
}

static void a_removed_timer_does_not_expire(void)
{
    struct timer_heap *timers = timers_make(4);
    struct timer kept;
    struct timer removed;
    timer_init(&kept);
    timer_init(&removed);
    CHECK(timer_heap_add(timers, &removed, 1));
    CHECK(timer_heap_add(timers, &kept, 2));
    timer_heap_remove(timers, &removed);
    CHECK(!timer_is_set(&removed));
    CHECK(timer_heap_remove_expired(timers, 5) == &kept);
    CHECK(timer_heap_remove_expired(timers, 5) == NULL);
    timer_heap_destroy(timers);
}

static void removing_a_timer_in_no_heap_does_nothing(void)
{
    struct timer_heap *timers = timers_make(4);
    struct timer kept;
    struct timer stray;
    timer_init(&kept);
    timer_init(&stray);
    CHECK(timer_heap_add(timers, &kept, 2));
    timer_heap_remove(timers, &stray);
    CHECK(timer_heap_find_first(timers) == &kept);
    timer_heap_destroy(timers);
}

static void a_full_heap_refuses_a_new_timer_but_moves_one_it_holds(void)
{
    struct timer_heap *timers = timers_make(2);
    struct timer first;
    struct timer second;
    struct timer third;
    timer_init(&first);
    timer_init(&second);
    timer_init(&third);
    CHECK(timer_heap_add(timers, &first, 5));
    CHECK(timer_heap_add(timers, &second, 6));
    CHECK(!timer_heap_add(timers, &third, 1));
    CHECK(!timer_is_set(&third));
    CHECK(timer_heap_find_first(timers) == &first);
    CHECK(timer_heap_add(timers, &second, 1));
    CHECK(timer_heap_find_first(timers) == &second);
    timer_heap_destroy(timers);
}

/* Deadlines from a fixed scramble of 0..99; every third timer leaves
   before the end. The rest must come out sorted. */
static void many_timers_come_out_sorted_after_removals(void)
{
    struct timer_heap *timers = timers_make(MANY);
    struct timer pool[MANY];
    for (int i = 0; i < MANY; i++) {
        timer_init(&pool[i]);
        CHECK(timer_heap_add(timers, &pool[i], (double)((i * 37) % MANY)));
    }
    for (int i = 0; i < MANY; i += 3) {
        timer_heap_remove(timers, &pool[i]);
    }
    double last = -1;
    int count = 0;
    struct timer *timer = NULL;
    while ((timer = timer_heap_remove_expired(timers, MANY)) != NULL) {
        CHECK(timer->deadline >= last);
        last = timer->deadline;
        count++;
    }
    CHECK_EQ_INT(count, 66);
    timer_heap_destroy(timers);
}

static void a_heap_of_zero_timers_is_not_made(void)
{
    CHECK(timer_heap_create(0) == NULL);
}

int main(void)
{
    an_empty_heap_has_no_first_timer();
    a_new_timer_is_in_no_heap();
    the_first_timer_has_the_earliest_deadline();
    adding_a_timer_again_moves_it();
    a_timer_expires_at_its_deadline_not_before();
    expired_timers_come_out_in_deadline_order();
    a_removed_timer_does_not_expire();
    removing_a_timer_in_no_heap_does_nothing();
    a_full_heap_refuses_a_new_timer_but_moves_one_it_holds();
    many_timers_come_out_sorted_after_removals();
    a_heap_of_zero_timers_is_not_made();
    printf("test_timer_heap: ok\n");

    return 0;
}
