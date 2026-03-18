/**
 * fiber_pool.h - Fiber pool interface for gsyncio
 * 
 * Object pool for efficient fiber allocation.
 */

#ifndef FIBER_POOL_H
#define FIBER_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "fiber.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================ */
/* Fiber Pool Structure                        */
/* ============================================ */

typedef struct fiber_pool {
    fiber_t* fibers;           /* Array of fiber control blocks */
    fiber_t** free_list;       /* Free list for O(1) allocation */
    size_t capacity;           /* Total capacity */
    size_t available;          /* Available fibers */
    size_t allocated;          /* Currently allocated */
    pthread_mutex_t mutex;     /* Thread safety */
} fiber_pool_t;

/* ============================================ */
/* Pool Lifecycle                              */
/* ============================================ */

/**
 * Create a new fiber pool
 * @param initial_size Initial number of fibers (0 = default)
 * @return New pool, or NULL on failure
 */
fiber_pool_t* fiber_pool_create(size_t initial_size);

/**
 * Destroy a fiber pool
 * @param pool Pool to destroy
 */
void fiber_pool_destroy(fiber_pool_t* pool);

/* ============================================ */
/* Allocation                                  */
/* ============================================ */

/**
 * Allocate a fiber from the pool
 * @param pool Pool to allocate from
 * @return Fiber, or NULL if pool exhausted
 */
fiber_t* fiber_pool_alloc(fiber_pool_t* pool);

/**
 * Free a fiber back to the pool
 * @param pool Pool
 * @param fiber Fiber to free
 */
void fiber_pool_free(fiber_pool_t* pool, fiber_t* fiber);

/* ============================================ */
/* Statistics                                  */
/* ============================================ */

size_t fiber_pool_available(fiber_pool_t* pool);
size_t fiber_pool_allocated(fiber_pool_t* pool);
size_t fiber_pool_capacity(fiber_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* FIBER_POOL_H */
