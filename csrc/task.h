/**
 * task.h - Task/Sync model interface for gsyncio
 *
 * Fire-and-forget parallel work with collective synchronization
 * (like Go's goroutines with sync.WaitGroup).
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
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

/* Wrapper argument for task execution */
typedef struct task_wrapper_arg {
    void (*func)(void*);          /* User function */
    void* arg;                     /* User argument */
    task_handle_t* handle;         /* Task handle */
    task_registry_t* registry;     /* Task registry */
} task_wrapper_arg_t;

/* Task handle */
struct task_handle {
    uint64_t fiber_id;            /* Fiber ID */
    task_state_t state;           /* Task state */
    void* result;                 /* Result (if any) */
    void* exception;              /* Exception (if any) */
    task_wrapper_arg_t* wrapper_arg;  /* Wrapper argument */
};

/* Task registry - tracks all active tasks */
struct task_registry {
    task_handle_t** tasks;        /* Array of task handles */
    size_t task_count;            /* Total tasks spawned */
    size_t task_capacity;         /* Array capacity */
    size_t active_count;          /* Currently active tasks */
    waitgroup_t* wg;              /* WaitGroup for synchronization */
    pthread_mutex_t mutex;        /* Thread safety */
    pthread_cond_t cond;          /* Condition for waiting */
};

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
 * Spawn a new task
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
 * @return Number of active tasks
 */
size_t task_count(task_registry_t* reg);

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

#ifdef __cplusplus
}
#endif

#endif /* TASK_H */
