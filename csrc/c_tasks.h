/**
 * c_tasks.h - C-based task execution for gsyncio
 * 
 * Provides C function callbacks that can be executed without GIL,
 * enabling true parallel execution across multiple worker threads.
 */

#ifndef C_TASKS_H
#define C_TASKS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================ */
/* Configuration                                */
/* ============================================ */

#define MAX_C_TASKS 1024
#define MAX_C_TASK_ARGS 8

/* ============================================ */
/* C Task Function Types                        */
/* ============================================ */

/**
 * C task function signature - no Python, no GIL needed
 * @param arg User-defined argument
 * @return Result code (0 = success)
 */
typedef int (*c_task_func_t)(void* arg);

/**
 * C task with integer argument
 * @param value Integer value
 * @return Result code
 */
typedef int (*c_task_func_int_t)(int value);

/**
 * C task with integer arguments
 * @param arg1 First integer
 * @param arg2 Second integer
 * @return Result code
 */
typedef int (*c_task_func_int_int_t)(int arg1, int arg2);

/* ============================================ */
/* Task Registry                                */
/* ============================================ */

typedef struct {
    c_task_func_t func;
    void* arg;
    const char* name;
    bool active;
} c_task_entry_t;

typedef struct {
    c_task_entry_t tasks[MAX_C_TASKS];
    size_t count;
    pthread_mutex_t mutex;
} c_task_registry_t;

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

/**
 * Initialize C task registry
 * @return 0 on success, -1 on failure
 */
int c_tasks_init(void);

/**
 * Shutdown C task registry
 */
void c_tasks_shutdown(void);

/* ============================================ */
/* Task Registration                            */
/* ============================================ */

/**
 * Register a C task function
 * @param name Task name (for lookup)
 * @param func Task function pointer
 * @return Task ID (>=0) on success, -1 on failure
 */
int c_task_register(const char* name, c_task_func_t func);

/**
 * Unregister a C task function
 * @param task_id Task ID
 * @return 0 on success, -1 on failure
 */
int c_task_unregister(int task_id);

/**
 * Lookup task by name
 * @param name Task name
 * @return Task ID (>=0) on success, -1 on failure
 */
int c_task_lookup(const char* name);

/* ============================================ */
/* Task Execution (GIL-free)                    */
/* ============================================ */

/**
 * Execute a registered C task
 * @param task_id Task ID
 * @param arg User argument
 * @return Result code from task function
 */
int c_task_execute(int task_id, void* arg);

/**
 * Spawn a C task on the scheduler (no GIL needed!)
 * @param task_id Task ID
 * @param arg User argument
 * @return Fiber ID on success, 0 on failure
 */
uint64_t c_task_spawn(int task_id, void* arg);

/**
 * Spawn a C task with integer argument
 * @param task_id Task ID
 * @param value Integer argument
 * @return Fiber ID on success, 0 on failure
 */
uint64_t c_task_spawn_int(int task_id, int value);

/**
 * Spawn a C task with two integer arguments
 * @param task_id Task ID
 * @param arg1 First integer argument
 * @param arg2 Second integer argument
 * @return Fiber ID on success, 0 on failure
 */
uint64_t c_task_spawn_int_int(int task_id, int arg1, int arg2);

/* ============================================ */
/* Pre-registered C Tasks                       */
/* ============================================ */

/**
 * CPU-bound computation: sum of squares
 * Computes: sum(i*i for i in range(n))
 * @param arg Pointer to int (n)
 * @return Result (stored in arg)
 */
int c_task_sum_squares(void* arg);

/**
 * CPU-bound computation: matrix multiplication
 * @param arg Pointer to matrix_multiply_args_t
 * @return 0 on success
 */
int c_task_matrix_multiply(void* arg);

/**
 * CPU-bound computation: prime counting
 * Counts primes up to n
 * @param arg Pointer to int (n)
 * @return Count of primes
 */
int c_task_count_primes(void* arg);

/**
 * Memory-bound computation: array fill
 * @param arg Pointer to array_fill_args_t
 * @return 0 on success
 */
int c_task_array_fill(void* arg);

/**
 * Memory-bound computation: array copy
 * @param arg Pointer to array_copy_args_t
 * @return 0 on success
 */
int c_task_array_copy(void* arg);

/* ============================================ */
/* Statistics                                   */
/* ============================================ */

typedef struct {
    uint64_t total_c_tasks_spawned;
    uint64_t total_c_tasks_completed;
    uint64_t total_c_task_time_ns;
    uint64_t total_python_tasks_spawned;
    uint64_t total_python_tasks_completed;
    uint64_t total_python_task_time_ns;
} c_task_stats_t;

/**
 * Get C task statistics
 * @param stats Pointer to stats struct
 */
void c_task_get_stats(c_task_stats_t* stats);

/**
 * Reset C task statistics
 */
void c_task_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* C_TASKS_H */
