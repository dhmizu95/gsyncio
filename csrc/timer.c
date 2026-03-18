#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "timer.h"

uint64_t timer_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

uint64_t timer_add_ns(uint64_t base, uint64_t ns) {
    return base + ns;
}

int timer_compare(const void *a, const void *b) {
    timer_t *ta = *(timer_t**)a;
    timer_t *tb = *(timer_t**)b;
    if (ta->expire_ns < tb->expire_ns) return -1;
    if (ta->expire_ns > tb->expire_ns) return 1;
    return 0;
}

static void swap(timer_heap_t *heap, size_t i, size_t j) {
    timer_t *temp = heap->timers[i];
    heap->timers[i] = heap->timers[j];
    heap->timers[j] = temp;
    
    heap->timers[i]->heap_index = i;
    heap->timers[j]->heap_index = j;
}

static void heapify_up(timer_heap_t *heap, size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (heap->timers[index]->expire_ns >= heap->timers[parent]->expire_ns) {
            break;
        }
        swap(heap, index, parent);
        index = parent;
    }
}

static void heapify_down(timer_heap_t *heap, size_t index) {
    size_t count = heap->count;
    
    while (true) {
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;
        size_t smallest = index;
        
        if (left < count && heap->timers[left]->expire_ns < heap->timers[smallest]->expire_ns) {
            smallest = left;
        }
        if (right < count && heap->timers[right]->expire_ns < heap->timers[smallest]->expire_ns) {
            smallest = right;
        }
        
        if (smallest == index) {
            break;
        }
        
        swap(heap, index, smallest);
        index = smallest;
    }
}

timer_heap_t* timer_heap_create(size_t capacity) {
    timer_heap_t *heap = (timer_heap_t*)calloc(1, sizeof(timer_heap_t));
    if (!heap) {
        return NULL;
    }
    
    heap->timers = (timer_t**)calloc(capacity, sizeof(timer_t*));
    if (!heap->timers) {
        free(heap);
        return NULL;
    }
    
    heap->capacity = capacity;
    heap->count = 0;
    
    return heap;
}

void timer_heap_destroy(timer_heap_t *heap) {
    if (!heap) {
        return;
    }
    
    free(heap->timers);
    free(heap);
}

int timer_heap_add(timer_heap_t *heap, timer_t *timer) {
    if (!heap || !timer || heap->count >= heap->capacity) {
        return -1;
    }
    
    timer->active = true;
    timer->heap_index = heap->count;
    heap->timers[heap->count] = timer;
    heap->count++;
    
    heapify_up(heap, heap->count - 1);
    
    return 0;
}

void timer_heap_remove(timer_heap_t *heap, timer_t *timer) {
    if (!heap || !timer || !timer->active) {
        return;
    }
    
    size_t index = timer->heap_index;
    size_t last = heap->count - 1;
    
    if (index != last) {
        swap(heap, index, last);
        heap->count--;
        
        if (index > 0) {
            size_t parent = (index - 1) / 2;
            if (heap->timers[index]->expire_ns < heap->timers[parent]->expire_ns) {
                heapify_up(heap, index);
            } else {
                heapify_down(heap, index);
            }
        }
    } else {
        heap->count--;
    }
    
    timer->active = false;
    timer->heap_index = -1;
}

void timer_heap_update(timer_heap_t *heap, timer_t *timer) {
    if (!heap || !timer || !timer->active) {
        return;
    }
    
    heapify_up(heap, timer->heap_index);
    heapify_down(heap, timer->heap_index);
}

timer_t* timer_heap_pop(timer_heap_t *heap) {
    if (!heap || heap->count == 0) {
        return NULL;
    }
    
    timer_t *timer = heap->timers[0];
    timer->active = false;
    timer->heap_index = -1;
    
    if (heap->count > 1) {
        swap(heap, 0, heap->count - 1);
        heap->count--;
        heapify_down(heap, 0);
    } else {
        heap->count--;
    }
    
    return timer;
}

timer_t* timer_heap_peek(timer_heap_t *heap) {
    if (!heap || heap->count == 0) {
        return NULL;
    }
    return heap->timers[0];
}
