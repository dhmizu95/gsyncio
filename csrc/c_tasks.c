/**
 * c_tasks.c - C-based task execution for gsyncio
 * 
 * Provides C function callbacks that can be executed without GIL,
 * enabling true parallel execution across multiple worker threads.
 */

#define _GNU_SOURCE  /* For strdup */

#include "c_tasks.h"
#include "scheduler.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <pthread.h>

/* ============================================ */
/* Global State                                 */
/* ============================================ */

static c_task_registry_t g_c_task_registry;
static c_task_stats_t g_c_task_stats;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

int c_tasks_init(void) {
    memset(&g_c_task_registry, 0, sizeof(g_c_task_registry));
    memset(&g_c_task_stats, 0, sizeof(g_c_task_stats));
    pthread_mutex_init(&g_c_task_registry.mutex, NULL);
    
    /* Pre-register common C tasks */
    c_task_register("sum_squares", c_task_sum_squares);
    c_task_register("count_primes", c_task_count_primes);
    c_task_register("array_fill", c_task_array_fill);
    c_task_register("array_copy", c_task_array_copy);
    
    return 0;
}

void c_tasks_shutdown(void) {
    pthread_mutex_destroy(&g_c_task_registry.mutex);
    pthread_mutex_destroy(&g_stats_mutex);
}

/* ============================================ */
/* Task Registration                            */
/* ============================================ */

int c_task_register(const char* name, c_task_func_t func) {
    if (!name || !func) {
        return -1;
    }
    
    pthread_mutex_lock(&g_c_task_registry.mutex);
    
    if (g_c_task_registry.count >= MAX_C_TASKS) {
        pthread_mutex_unlock(&g_c_task_registry.mutex);
        return -1;
    }
    
    /* Find empty slot or add to end */
    int slot = -1;
    for (size_t i = 0; i < g_c_task_registry.count; i++) {
        if (!g_c_task_registry.tasks[i].active) {
            slot = (int)i;
            break;
        }
    }
    
    if (slot < 0) {
        slot = (int)g_c_task_registry.count;
        g_c_task_registry.count++;
    }
    
    g_c_task_registry.tasks[slot].func = func;
    g_c_task_registry.tasks[slot].arg = NULL;
    g_c_task_registry.tasks[slot].name = strdup(name);
    g_c_task_registry.tasks[slot].active = true;
    
    pthread_mutex_unlock(&g_c_task_registry.mutex);
    return slot;
}

int c_task_unregister(int task_id) {
    if (task_id < 0 || task_id >= (int)g_c_task_registry.count) {
        return -1;
    }
    
    pthread_mutex_lock(&g_c_task_registry.mutex);
    
    if (!g_c_task_registry.tasks[task_id].active) {
        pthread_mutex_unlock(&g_c_task_registry.mutex);
        return -1;
    }
    
    free((void*)g_c_task_registry.tasks[task_id].name);
    g_c_task_registry.tasks[task_id].active = false;
    
    pthread_mutex_unlock(&g_c_task_registry.mutex);
    return 0;
}

int c_task_lookup(const char* name) {
    if (!name) {
        return -1;
    }
    
    pthread_mutex_lock(&g_c_task_registry.mutex);
    
    for (size_t i = 0; i < g_c_task_registry.count; i++) {
        if (g_c_task_registry.tasks[i].active && 
            g_c_task_registry.tasks[i].name &&
            strcmp(g_c_task_registry.tasks[i].name, name) == 0) {
            pthread_mutex_unlock(&g_c_task_registry.mutex);
            return (int)i;
        }
    }
    
    pthread_mutex_unlock(&g_c_task_registry.mutex);
    return -1;
}

/* ============================================ */
/* Task Execution (GIL-free)                    */
/* ============================================ */

/* Wrapper for C task execution */
typedef struct {
    c_task_func_t func;
    void* arg;
    int result;
} c_task_wrapper_t;

static void c_task_wrapper(void* arg) {
    c_task_wrapper_t* wrapper = (c_task_wrapper_t*)arg;
    wrapper->result = wrapper->func(wrapper->arg);
}

int c_task_execute(int task_id, void* arg) {
    if (task_id < 0 || task_id >= (int)g_c_task_registry.count) {
        return -1;
    }
    
    pthread_mutex_lock(&g_c_task_registry.mutex);
    
    if (!g_c_task_registry.tasks[task_id].active) {
        pthread_mutex_unlock(&g_c_task_registry.mutex);
        return -1;
    }
    
    c_task_func_t func = g_c_task_registry.tasks[task_id].func;
    pthread_mutex_unlock(&g_c_task_registry.mutex);
    
    return func(arg);
}

uint64_t c_task_spawn(int task_id, void* arg) {
    if (task_id < 0 || task_id >= (int)g_c_task_registry.count) {
        return 0;
    }
    
    pthread_mutex_lock(&g_c_task_registry.mutex);
    
    if (!g_c_task_registry.tasks[task_id].active) {
        pthread_mutex_unlock(&g_c_task_registry.mutex);
        return 0;
    }
    
    c_task_func_t func = g_c_task_registry.tasks[task_id].func;
    pthread_mutex_unlock(&g_c_task_registry.mutex);
    
    /* Allocate wrapper */
    c_task_wrapper_t* wrapper = (c_task_wrapper_t*)malloc(sizeof(c_task_wrapper_t));
    if (!wrapper) {
        return 0;
    }
    wrapper->func = func;
    wrapper->arg = arg;
    wrapper->result = 0;
    
    /* Spawn fiber - NO GIL NEEDED! */
    uint64_t fid = scheduler_spawn(c_task_wrapper, wrapper);
    
    if (fid > 0) {
        pthread_mutex_lock(&g_stats_mutex);
        g_c_task_stats.total_c_tasks_spawned++;
        pthread_mutex_unlock(&g_stats_mutex);
    }
    
    return fid;
}

uint64_t c_task_spawn_int(int task_id, int value) {
    /* Box integer in heap */
    int* arg = (int*)malloc(sizeof(int));
    if (!arg) {
        return 0;
    }
    *arg = value;
    return c_task_spawn(task_id, arg);
}

uint64_t c_task_spawn_int_int(int task_id, int arg1, int arg2) {
    /* Pack two integers */
    int* args = (int*)malloc(2 * sizeof(int));
    if (!args) {
        return 0;
    }
    args[0] = arg1;
    args[1] = arg2;
    return c_task_spawn(task_id, args);
}

/* ============================================ */
/* Pre-registered C Tasks                       */
/* ============================================ */

int c_task_sum_squares(void* arg) {
    if (!arg) return -1;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int n = *(int*)arg;
    long long sum = 0;
    
    /* Pure C computation - NO GIL! */
    for (int i = 0; i < n; i++) {
        sum += (long long)i * i;
    }
    
    /* Store result back in arg */
    *(long long*)arg = sum;
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    uint64_t elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL + 
                          (end.tv_nsec - start.tv_nsec);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_c_task_stats.total_c_task_time_ns += elapsed_ns;
    g_c_task_stats.total_c_tasks_completed++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return 0;
}

int c_task_count_primes(void* arg) {
    if (!arg) return -1;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int n = *(int*)arg;
    int count = 0;
    
    /* Simple prime counting - NO GIL! */
    for (int num = 2; num <= n; num++) {
        int is_prime = 1;
        for (int i = 2; i * i <= num; i++) {
            if (num % i == 0) {
                is_prime = 0;
                break;
            }
        }
        if (is_prime) {
            count++;
        }
    }
    
    /* Store result */
    *(int*)arg = count;
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    uint64_t elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL + 
                          (end.tv_nsec - start.tv_nsec);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_c_task_stats.total_c_task_time_ns += elapsed_ns;
    g_c_task_stats.total_c_tasks_completed++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return count;
}

int c_task_array_fill(void* arg) {
    if (!arg) return -1;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* arg is [size, *array] */
    int* args = (int*)arg;
    int size = args[0];
    int* array = (int*)(args + 1);
    
    /* Memory operation - NO GIL! */
    for (int i = 0; i < size; i++) {
        array[i] = i * i;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    uint64_t elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL + 
                          (end.tv_nsec - start.tv_nsec);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_c_task_stats.total_c_task_time_ns += elapsed_ns;
    g_c_task_stats.total_c_tasks_completed++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return 0;
}

int c_task_array_copy(void* arg) {
    if (!arg) return -1;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* arg is [size, *src, *dst] */
    int* args = (int*)arg;
    int size = args[0];
    int* src = (int*)(args + 1);
    int* dst = (int*)(args + 1 + size);
    
    /* Memory operation - NO GIL! */
    memcpy(dst, src, size * sizeof(int));
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    uint64_t elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL + 
                          (end.tv_nsec - start.tv_nsec);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_c_task_stats.total_c_task_time_ns += elapsed_ns;
    g_c_task_stats.total_c_tasks_completed++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return 0;
}

/* ============================================ */
/* Statistics                                   */
/* ============================================ */

void c_task_get_stats(c_task_stats_t* stats) {
    if (!stats) return;
    
    pthread_mutex_lock(&g_stats_mutex);
    *stats = g_c_task_stats;
    pthread_mutex_unlock(&g_stats_mutex);
}

void c_task_reset_stats(void) {
    pthread_mutex_lock(&g_stats_mutex);
    memset(&g_c_task_stats, 0, sizeof(g_c_task_stats));
    pthread_mutex_unlock(&g_stats_mutex);
}
