/**
 * future.h - Future implementation for gsyncio
 * 
 * A Future represents a value that will be available in the future.
 * Used for async/await operations.
 */

#ifndef FUTURE_H
#define FUTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "fiber.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Future states */
typedef enum future_state {
    FUTURE_PENDING,   /* Not yet completed */
    FUTURE_READY,     /* Completed successfully */
    FUTURE_EXCEPTION  /* Completed with exception */
} future_state_t;

/* Future structure */
typedef struct future {
    future_state_t state;       /* Current state */
    void* result;               /* Result value (Python object) */
    void* exception;            /* Exception (Python object) */
    
    /* Callbacks */
    void** callbacks;           /* Array of callback functions */
    size_t callback_count;      /* Number of callbacks */
    size_t callback_capacity;   /* Callback array capacity */
    
    /* Waiting fibers */
    fiber_t* waiting_fibers; /* Linked list of fibers waiting on this future */
    
    /* Reference counting */
    int refcount;               /* Reference count */
    
    /* Synchronization */
    pthread_mutex_t mutex;      /* Thread safety */
    pthread_cond_t cond;        /* Condition for waiting */
} future_t;

/* Callback function type */
typedef void (*future_callback_t)(future_t* f, void* user_data);

/* Future API */

/**
 * Create a new future
 * @return New future, or NULL on error
 */
future_t* future_create(void);

/**
 * Destroy a future
 * @param f Future to destroy
 */
void future_destroy(future_t* f);

/**
 * Increment reference count
 * @param f Future
 */
void future_incref(future_t* f);

/**
 * Decrement reference count and destroy if zero
 * @param f Future
 */
void future_decref(future_t* f);

/**
 * Check if future is done
 * @param f Future
 * @return true if future is ready or has exception
 */
bool future_is_done(future_t* f);

/**
 * Check if future has exception
 * @param f Future
 * @return true if future has exception
 */
bool future_has_exception(future_t* f);

/**
 * Set future result
 * @param f Future
 * @param result Result value (Python object)
 * @return 0 on success, -1 on error
 */
int future_set_result(future_t* f, void* result);

/**
 * Get future result (blocks if not ready)
 * @param f Future
 * @return Result value, or NULL on error/exception
 */
void* future_result(future_t* f);

/**
 * Get future result without blocking
 * @param f Future
 * @param out_result Output for result
 * @return 0 if ready, -1 if still pending
 */
int future_result_nowait(future_t* f, void** out_result);

/**
 * Set future exception
 * @param f Future
 * @param exc Exception object (Python object)
 * @return 0 on success, -1 on error
 */
int future_set_exception(future_t* f, void* exc);

/**
 * Get future exception
 * @param f Future
 * @return Exception object, or NULL if no exception
 */
void* future_exception(future_t* f);

/**
 * Add callback to future
 * @param f Future
 * @param callback Callback function
 * @param user_data User data for callback
 * @return 0 on success, -1 on error
 */
int future_add_callback(future_t* f, future_callback_t callback, void* user_data);

/**
 * Wait for future to complete
 * @param f Future
 */
void future_wait(future_t* f);

/**
 * Park current fiber waiting for future
 * @param f Future
 */
void future_await(future_t* f);

#ifdef __cplusplus
}
#endif

#endif /* FUTURE_H */
