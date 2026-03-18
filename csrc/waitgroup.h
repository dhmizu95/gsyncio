/**
 * waitgroup.h - WaitGroup interface for gsyncio
 * 
 * Synchronization primitive for waiting on multiple fiber completions
 * (like Go's sync.WaitGroup).
 */

#ifndef WAITGROUP_H
#define WAITGROUP_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct fiber fiber_t;

/* WaitGroup structure */
typedef struct waitgroup {
    int64_t counter;            /* Internal counter */
    
    /* Waiting fibers */
    fiber_t* waiting_fibers;    /* Linked list of fibers waiting */
    
    /* Reference counting */
    int refcount;               /* Reference count */
    
    /* Synchronization */
    pthread_mutex_t mutex;      /* Thread safety */
    pthread_cond_t cond;        /* Condition for waiting */
} waitgroup_t;

/* WaitGroup API */

/**
 * Create a new waitgroup
 * @return New waitgroup, or NULL on error
 */
waitgroup_t* waitgroup_create(void);

/**
 * Destroy a waitgroup
 * @param wg WaitGroup to destroy
 */
void waitgroup_destroy(waitgroup_t* wg);

/**
 * Increment reference count
 * @param wg WaitGroup
 */
void waitgroup_incref(waitgroup_t* wg);

/**
 * Decrement reference count and destroy if zero
 * @param wg WaitGroup
 */
void waitgroup_decref(waitgroup_t* wg);

/**
 * Add delta to the waitgroup counter
 * @param wg WaitGroup
 * @param delta Value to add (can be negative)
 * @return 0 on success, -1 on error
 */
int waitgroup_add(waitgroup_t* wg, int64_t delta);

/**
 * Decrement the waitgroup counter (signal completion)
 * @param wg WaitGroup
 */
void waitgroup_done(waitgroup_t* wg);

/**
 * Wait for the waitgroup counter to reach zero
 * @param wg WaitGroup
 */
void waitgroup_wait(waitgroup_t* wg);

/**
 * Get current counter value
 * @param wg WaitGroup
 * @return Current counter value
 */
int64_t waitgroup_counter(waitgroup_t* wg);

#ifdef __cplusplus
}
#endif

#endif /* WAITGROUP_H */
