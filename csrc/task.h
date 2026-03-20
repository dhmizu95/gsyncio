/**
 * task.h - Task/Sync model interface for gsyncio
 *
 * Fire-and-forget parallel work with collective synchronization.
 * Optimized C implementation with batch spawning and lock-free tracking.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include "waitgroup.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Task states */
typedef enum task_state {
    TASK_STATE_RUNNING,
    TASK_STATE_COMPLETED,
    TASK_STATE_CANCELLED
} task_state_t;

/* Forward declarations */
typedef struct task_handle task_handle_t;
typedef struct task_registry task_registry_t;
typedef struct task_batch task_batch_t;

/* Task handle */
struct task_handle {
    uint64_t fiber_id;            /* Fiber ID */
    uint64_t task_id;             /* Task ID within registry */
    task_state_t state;           /* Task state */
    void* result;                 /* Result (if any) */
    void* exception;              /* Exception (if any) */
    void* wrapper_arg;            /* Internal wrapper argument */
};

/* Task registry - tracks all active tasks */
struct task_registry {
    _Atomic size_t task_count;       /* Total tasks spawned */
    _Atomic size_t active_count;     /* Currently active tasks */
    _Atomic size_t completion_count; /* Completed tasks */
    
    task_handle_t** tasks;           /* Array of task handles (optional) */
    size_t task_capacity;            /* Array capacity */
    
    waitgroup_t* wg;                 /* WaitGroup for synchronization */
    pthread_mutex_t mutex;           /* Thread safety */
    pthread_cond_t cond;             /* Condition for waiting */
    int waiters;                     /* Number of waiting threads */
    int all_done;                    /* Flag: all tasks completed */
};

/* Task batch for bulk spawning */
struct task_batch {
    void (**funcs)(void*);       /* Array of functions */
    void** args;                  /* Array of arguments */
    task_handle_t** handles;      /* Array of task handles */
    size_t count;                 /* Number of tasks in batch */
    size_t capacity;              /* Array capacity */
    task_registry_t* registry;    /* Associated registry */
};

/* ============================================ */
/* Ultra-fast batch spawn (no Python overhead) */
/* ============================================ */

/**
 * Fast batch spawn context - stores all data in C arrays
 * This structure is designed for zero Python overhead during spawn
 */
typedef struct task_batch_fast {
    void (**funcs)(void*);       /* C function pointers */
    void** args;                  /* Pre-stored arguments (Python objects as void*) */
    uint64_t* fiber_ids;          /* Optional: store fiber IDs */
    size_t count;
    size_t capacity;
    int store_fiber_ids;          /* Whether to store fiber IDs */
} task_batch_fast_t;

/**
 * Create a fast batch spawn context
 * @param capacity Initial capacity
 * @return Fast batch context, or NULL on error
 */
task_batch_fast_t* task_batch_fast_create(size_t capacity);

/**
 * Destroy a fast batch spawn context
 * @param batch Fast batch context
 * @param free_args Whether to free argument storage (0 = no, 1 = yes)
 */
void task_batch_fast_destroy(task_batch_fast_t* batch, int free_args);

/**
 * Add a task to fast batch (C function pointer + void* arg)
 * @param batch Fast batch context
 * @param func C function pointer
 * @param arg Void pointer argument
 * @return 0 on success, -1 on error
 */
int task_batch_fast_add(task_batch_fast_t* batch, void (*func)(void*), void* arg);

/**
 * Spawn all tasks in fast batch - NO GIL held during spawn
 * This is the key optimization - entire spawn loop runs without GIL
 * @param batch Fast batch context
 * @param registry Task registry
 * @return Number of tasks spawned
 */
size_t task_batch_fast_spawn_nogil(task_batch_fast_t* batch, task_registry_t* registry);

/* Task registry API */

/**
 * Create a new task registry
 * @return New task registry, or NULL on error
 */
task_registry_t* task_registry_create(void);

/**
 * Destroy a task registry
 * @param reg Task registry
 */
void task_registry_destroy(task_registry_t* reg);

/**
 * Spawn a single task
 * @param reg Task registry
 * @param func Function to execute
 * @param arg Argument to pass to function
 * @return Task handle, or NULL on error
 */
task_handle_t* task_spawn(task_registry_t* reg, void (*func)(void*), void* arg);

/**
 * Task wrapper function (internal)
 * @param arg Wrapper argument
 */
void task_wrapper(void* arg);

/**
 * Create a task batch for bulk spawning
 * @param capacity Initial capacity
 * @return Task batch, or NULL on error
 */
task_batch_t* task_batch_create(size_t capacity);

/**
 * Destroy a task batch
 * @param batch Task batch
 */
void task_batch_destroy(task_batch_t* batch);

/**
 * Add a task to a batch
 * @param batch Task batch
 * @param func Function to execute
 * @param arg Argument to pass to function
 * @return 0 on success, -1 on error
 */
int task_batch_add(task_batch_t* batch, void (*func)(void*), void* arg);

/**
 * Spawn all tasks in a batch (optimized bulk spawn)
 * @param batch Task batch
 * @param reg Task registry
 * @return 0 on success, -1 on error
 */
int task_batch_spawn(task_batch_t* batch, task_registry_t* reg);

/**
 * Wait for all tasks to complete
 * @param reg Task registry
 */
void task_sync(task_registry_t* reg);

/**
 * Wait for all tasks with timeout
 * @param reg Task registry
 * @param timeout_ns Timeout in nanoseconds
 * @return 0 if all completed, -1 on timeout
 */
int task_sync_timeout(task_registry_t* reg, uint64_t timeout_ns);

/**
 * Get number of active tasks
 * @param reg Task registry
 * @return Number of active tasks (atomic)
 */
size_t task_count(task_registry_t* reg);

/**
 * Get number of completed tasks
 * @param reg Task registry
 * @return Number of completed tasks (atomic)
 */
size_t task_completed_count(task_registry_t* reg);

/**
 * Get global task registry
 * @return Task registry
 */
task_registry_t* task_get_registry(void);

/**
 * Set global task registry
 * @param reg Task registry
 */
void task_set_registry(task_registry_t* reg);

/**
 * Reset task registry state (for reuse)
 * @param reg Task registry
 */
void task_reset_registry(task_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* TASK_H */
