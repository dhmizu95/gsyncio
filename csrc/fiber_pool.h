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

/* Number of per-worker shards. Matches scheduler.h's NUM_SHARDS by
 * convention (not a hard dependency - fiber_pool.h stays decoupled from
 * scheduler.h). Allocation and free both pick a shard from the current
 * thread's worker ID (see scheduler_get_current_worker_id()), so a
 * worker recycling the fiber it just ran, and whichever thread later
 * allocates for that same worker, contend on one 1-of-64 lock instead
 * of a single pool-wide mutex. */
#define FIBER_POOL_NUM_SHARDS 64

typedef struct fiber_pool_shard {
    _Atomic(void*) free_list;  /* Linked list of available fibers (reusing fiber->next_ready) */
    _Atomic size_t available;  /* Available fibers in this shard's free list */
    pthread_mutex_t mutex;     /* Protects this shard's free list only */
} __attribute__((aligned(64))) fiber_pool_shard_t;  /* cache-line aligned: avoid false sharing between shards */

typedef struct fiber_pool {
    fiber_pool_shard_t shards[FIBER_POOL_NUM_SHARDS];
    _Atomic size_t capacity;    /* Total fibers created across all shards (lock-free growth counter) */
    _Atomic size_t allocated;   /* Currently handed out fibers, across all shards */
    fiber_stack_mode_t stack_mode; /* Native vs Hybrid */
} fiber_pool_t;

/* ============================================ */
/* Pool Lifecycle                              */
/* ============================================ */

/**
 * Create a new fiber pool
 * @param initial_size Initial number of fibers (0 = default)
 * @param stack_mode Stack management mode
 * @return New pool, or NULL on failure
 */
fiber_pool_t* fiber_pool_create(size_t initial_size, fiber_stack_mode_t stack_mode);

/**
 * Destroy a fiber pool
 * @param pool Pool to destroy
 */
void fiber_pool_destroy(fiber_pool_t* pool);

/* ============================================ */
/* Allocation                                  */
/* ============================================ */

/**
 * Allocate a fiber from the pool.
 *
 * @param pool Pool to allocate from
 * @param worker_hint The worker ID that will actually run this fiber
 *   (i.e. what scheduler_schedule() will be called with), or -1 if not
 *   known yet. Passing the real target worker matters: fiber_pool_free()
 *   always returns a fiber to the shard matching whichever worker ran
 *   it, so allocating from that SAME shard is what makes fibers actually
 *   get reused. Allocating from a mismatched shard (e.g. always the
 *   calling thread's own shard when the caller isn't the target worker)
 *   starves that shard's free list, forcing constant pool growth while
 *   the correct shard's freed fibers just pile up unused.
 * @return Fiber, or NULL if pool exhausted
 */
fiber_t* fiber_pool_alloc(fiber_pool_t* pool, int worker_hint);

/**
 * Free a fiber back to the pool. Picks a shard the same way as
 * fiber_pool_alloc() - in practice this means a worker thread returns a
 * fiber to its own shard right after running it, uncontended by other
 * workers doing the same.
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

/**
 * Verify pool counter consistency
 * @param pool Pool to verify
 * @return 1 if consistent, 0 if inconsistent (debug only)
 */
int fiber_pool_verify_counters(fiber_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* FIBER_POOL_H */
