/**
 * fiber_pool.c - Fiber pool implementation for gsyncio
 * 
 * Object pool for efficient fiber allocation.
 * Pre-allocates fiber control blocks and stacks to reduce allocation overhead.
 * Provides O(1) allocation and deallocation with lazy stack initialization.
 */

#include "fiber_pool.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>

/* ============================================ */
/* Configuration                               */
/* ============================================ */

#define FIBER_POOL_INITIAL_SIZE 4096     /* Pre-allocate 4K fibers */
#define FIBER_POOL_GROWTH_FACTOR 2
#define FIBER_POOL_MAX_SIZE (1024 * 1024)  /* 1M fibers */
#define FIBER_POOL_LAZY_STACK 0         /* Pre-allocate stacks at pool creation */

/* ============================================ */
/* Fiber Pool Implementation                   */
/* ============================================ */

fiber_pool_t* fiber_pool_create(size_t initial_size) {
    if (initial_size == 0) {
        initial_size = FIBER_POOL_INITIAL_SIZE;
    }
    if (initial_size > FIBER_POOL_MAX_SIZE) {
        initial_size = FIBER_POOL_MAX_SIZE;
    }
    
    fiber_pool_t* pool = (fiber_pool_t*)calloc(1, sizeof(fiber_pool_t));
    if (!pool) {
        return NULL;
    }
    
    pool->capacity = initial_size;
    
    /* Pre-allocate fiber control blocks */
    pool->fibers = (fiber_t*)calloc(pool->capacity, sizeof(fiber_t));
    if (!pool->fibers) {
        free(pool);
        return NULL;
    }
    
    /* Initialize free list */
    pool->free_list = (fiber_t**)calloc(pool->capacity, sizeof(fiber_t*));
    if (!pool->free_list) {
        free(pool->fibers);
        free(pool);
        return NULL;
    }
    
    /* Pre-allocate stacks for all fibers (optional - can be disabled for memory) */
#if FIBER_POOL_LAZY_STACK == 0
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->fibers[i].id = i;
        pool->fibers[i].pool = pool;
        pool->fibers[i].stack_base = mmap(
            NULL,
            FIBER_DEFAULT_STACK_SIZE + 4096,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        );
        if (pool->fibers[i].stack_base != MAP_FAILED) {
            mprotect(pool->fibers[i].stack_base, 4096, PROT_NONE);
            pool->fibers[i].stack_capacity = FIBER_DEFAULT_STACK_SIZE;
            pool->fibers[i].stack_ptr = (char*)pool->fibers[i].stack_base + FIBER_DEFAULT_STACK_SIZE + 4096;
        }
        pool->free_list[i] = &pool->fibers[i];
    }
#else
    /* Lazy stack allocation - just initialize control blocks */
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->fibers[i].id = i;
        pool->fibers[i].pool = pool;
        pool->fibers[i].stack_base = NULL;
        pool->fibers[i].stack_capacity = 0;
        pool->free_list[i] = &pool->fibers[i];
    }
#endif
    
    pool->available = pool->capacity;
    pool->allocated = 0;
    
    pthread_mutex_init(&pool->mutex, NULL);
    
    return pool;
}

void fiber_pool_destroy(fiber_pool_t* pool) {
    if (!pool) {
        return;
    }
    
    pthread_mutex_destroy(&pool->mutex);
    free(pool->free_list);
    free(pool->fibers);
    free(pool);
}

fiber_t* fiber_pool_alloc(fiber_pool_t* pool) {
    if (!pool) {
        return NULL;
    }
    
    pthread_mutex_lock(&pool->mutex);
    
    if (pool->available == 0) {
        /* Grow the pool */
        size_t new_size = pool->capacity * FIBER_POOL_GROWTH_FACTOR;
        if (new_size > FIBER_POOL_MAX_SIZE) {
            new_size = FIBER_POOL_MAX_SIZE;
        }
        if (new_size == pool->capacity) {
            /* At max capacity */
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        
        /* Reallocate */
        fiber_t* new_fibers = (fiber_t*)realloc(pool->fibers, new_size * sizeof(fiber_t));
        if (!new_fibers) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        pool->fibers = new_fibers;
        
        fiber_t** new_free_list = (fiber_t**)realloc(pool->free_list, new_size * sizeof(fiber_t*));
        if (!new_free_list) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        pool->free_list = new_free_list;
        
        /* Initialize new fibers */
        for (size_t i = pool->capacity; i < new_size; i++) {
            pool->free_list[pool->available + (i - pool->capacity)] = &pool->fibers[i];
            pool->fibers[i].id = i;
            pool->fibers[i].pool = pool;
        }
        
        pool->available += (new_size - pool->capacity);
        pool->capacity = new_size;
    }
    
    /* Allocate from free list */
    pool->available--;
    fiber_t* fiber = pool->free_list[pool->available];
    pool->allocated++;
    
    /* Reset fiber state (preserve pre-allocated stack) */
    void* saved_stack_base = fiber->stack_base;
    size_t saved_stack_capacity = fiber->stack_capacity;
    char* saved_stack_ptr = fiber->stack_ptr;
    
    memset(fiber, 0, sizeof(fiber_t));
    
    /* Restore pre-allocated stack */
    fiber->stack_base = saved_stack_base;
    fiber->stack_capacity = saved_stack_capacity;
    fiber->stack_ptr = saved_stack_ptr;
    
    fiber->id = pool->allocated;  /* Unique ID */
    fiber->pool = pool;
    fiber->state = FIBER_NEW;
    
    pthread_mutex_unlock(&pool->mutex);
    
    return fiber;
}

void fiber_pool_free(fiber_pool_t* pool, fiber_t* fiber) {
    if (!pool || !fiber) {
        return;
    }
    
    pthread_mutex_lock(&pool->mutex);
    
    /* Reset fiber state for reuse (keep pre-allocated stack) */
    fiber->state = FIBER_NEW;
    fiber->func = NULL;
    fiber->arg = NULL;
    fiber->result = NULL;
    fiber->parent = NULL;
    fiber->next_ready = NULL;
    fiber->prev_ready = NULL;
    fiber->affinity = 0;
    fiber->waiting_on = NULL;
    
    /* Return to free list */
    pool->free_list[pool->available] = fiber;
    pool->available++;
    pool->allocated--;
    
    pthread_mutex_unlock(&pool->mutex);
}

size_t fiber_pool_available(fiber_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return pool->available;
}

size_t fiber_pool_allocated(fiber_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return pool->allocated;
}

size_t fiber_pool_capacity(fiber_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return pool->capacity;
}
