#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct timer_s timer_t;
typedef struct timer_heap_s timer_heap_t;

typedef void (*timer_callback_t)(void *arg);

struct timer_s {
    uint64_t expire_ns;
    uint64_t interval_ns;
    timer_callback_t callback;
    void *arg;
    bool active;
    bool repeating;
    
    int heap_index;
};

struct timer_heap_s {
    timer_t **timers;
    size_t capacity;
    size_t count;
};

timer_heap_t* timer_heap_create(size_t capacity);
void timer_heap_destroy(timer_heap_t *heap);

int timer_heap_add(timer_heap_t *heap, timer_t *timer);
void timer_heap_remove(timer_heap_t *heap, timer_t *timer);
void timer_heap_update(timer_heap_t *heap, timer_t *timer);
timer_t* timer_heap_pop(timer_heap_t *heap);
timer_t* timer_heap_peek(timer_heap_t *heap);

uint64_t timer_now_ns(void);
uint64_t timer_add_ns(uint64_t base, uint64_t ns);
int timer_compare(const timer_t *a, const timer_t *b);

#endif
