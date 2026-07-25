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
#include <stdatomic.h>

/* ============================================ */
/* Global State                                 */
/* ============================================ */

static c_task_registry_t g_c_task_registry;

/* Stats are sharded per-worker (same pattern as the scheduler's sharded
 * task/completion counters and the fiber pool's per-worker free lists):
 * every task completion used to take one global mutex, which turned
 * into real contention once spawning/pooling overhead was cut down by
 * the other fixes here. Each shard is cache-line aligned to avoid false
 * sharing between shards updated by different cores. */
#define C_TASK_STATS_NUM_SHARDS 64

typedef struct {
    _Atomic uint64_t c_tasks_spawned;
    _Atomic uint64_t c_tasks_completed;
    _Atomic uint64_t c_task_time_ns;
} __attribute__((aligned(64))) c_task_stats_shard_t;

static c_task_stats_shard_t g_stats_shards[C_TASK_STATS_NUM_SHARDS];

static inline c_task_stats_shard_t* current_stats_shard(void) {
    return &g_stats_shards[scheduler_get_current_worker_id() % C_TASK_STATS_NUM_SHARDS];
}

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

int c_tasks_init(void) {
    memset(&g_c_task_registry, 0, sizeof(g_c_task_registry));
    memset(&g_stats_shards, 0, sizeof(g_stats_shards));
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

/* A batch's wrappers and arg boxes live in one arena (one malloc each
 * instead of two mallocs per task). Fibers from the same batch finish
 * at different times, so the arena can't be freed until the LAST one
 * completes - `remaining` tracks that with a simple atomic refcount. */
typedef struct c_task_arena {
    void* wrappers;          /* count * sizeof(c_task_wrapper_t), see below */
    int* args;                /* count ints, one per task */
    _Atomic size_t remaining; /* fibers not yet finished (or never spawned) */
} c_task_arena_t;

/* Wrapper for C task execution */
typedef struct {
    c_task_func_t func;
    void* arg;
    int result;
    c_task_arena_t* arena;  /* NULL for individually-malloc'd wrappers */
} c_task_wrapper_t;

static void arena_release(c_task_arena_t* arena) {
    if (atomic_fetch_sub(&arena->remaining, 1) == 1) {
        free(arena->wrappers);
        free(arena->args);
        free(arena);
    }
}

static void c_task_wrapper(void* arg) {
    c_task_wrapper_t* wrapper = (c_task_wrapper_t*)arg;
    wrapper->func(wrapper->arg);
    /* Nothing reads wrapper->result back (fire-and-forget). */
    if (wrapper->arena) {
        /* wrapper/arg are slices of the batch arena, not their own
         * allocation - release our slot instead of freeing directly. */
        arena_release(wrapper->arena);
    } else {
        /* Individually malloc'd (c_task_spawn/_int/_int_int path) -
         * nobody else owns these, free them here or they leak forever. */
        free(wrapper->arg);
        free(wrapper);
    }
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
    wrapper->arena = NULL;

    /* Spawn fiber - NO GIL NEEDED! */
    uint64_t fid = scheduler_spawn(c_task_wrapper, wrapper);
    
    if (fid > 0) {
        atomic_fetch_add(&current_stats_shard()->c_tasks_spawned, 1);
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

size_t c_task_spawn_batch_int(int task_id, const int* values, size_t count) {
    if (task_id < 0 || task_id >= (int)g_c_task_registry.count || !values || count == 0) {
        return 0;
    }

    /* Resolve the function pointer ONCE for the whole batch, instead of
     * re-locking the registry mutex and re-comparing task_id on every
     * single spawn like repeated c_task_spawn_int() calls would. */
    pthread_mutex_lock(&g_c_task_registry.mutex);
    if (!g_c_task_registry.tasks[task_id].active) {
        pthread_mutex_unlock(&g_c_task_registry.mutex);
        return 0;
    }
    c_task_func_t func = g_c_task_registry.tasks[task_id].func;
    pthread_mutex_unlock(&g_c_task_registry.mutex);

    /* One arena for the whole batch: 3 mallocs total instead of 2*count.
     * Each task gets a slice (wrappers[i], args[i]) instead of its own
     * malloc'd wrapper + arg box. */
    c_task_arena_t* arena = (c_task_arena_t*)malloc(sizeof(c_task_arena_t));
    if (!arena) {
        return 0;
    }
    c_task_wrapper_t* wrappers = (c_task_wrapper_t*)malloc(count * sizeof(c_task_wrapper_t));
    int* args = (int*)malloc(count * sizeof(int));
    if (!wrappers || !args) {
        free(wrappers);
        free(args);
        free(arena);
        return 0;
    }
    arena->wrappers = wrappers;
    arena->args = args;
    memcpy(args, values, count * sizeof(int));
    /* Optimistic: assume every slot spawns. Slots that fail to spawn
     * (scheduler_spawn returns 0, so c_task_wrapper never runs for them)
     * release their own share immediately below instead. */
    atomic_store(&arena->remaining, count);

    size_t spawned = 0;
    for (size_t i = 0; i < count; i++) {
        c_task_wrapper_t* wrapper = &wrappers[i];
        wrapper->func = func;
        wrapper->arg = &args[i];
        wrapper->result = 0;
        wrapper->arena = arena;

        if (scheduler_spawn(c_task_wrapper, wrapper) > 0) {
            spawned++;
        } else {
            arena_release(arena);
        }
    }

    if (spawned > 0) {
        atomic_fetch_add(&current_stats_shard()->c_tasks_spawned, spawned);
    }

    return spawned;
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
    
    /* Do NOT write `sum` back into arg: arg was allocated as sizeof(int)
     * by c_task_spawn_int, and sum is a long long - writing it back
     * would overflow that 4-byte buffer and corrupt the heap. Nothing
     * reads this result back, so just return it instead. */

    clock_gettime(CLOCK_MONOTONIC, &end);

    uint64_t elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
                          (end.tv_nsec - start.tv_nsec);

    c_task_stats_shard_t* shard = current_stats_shard();
    atomic_fetch_add(&shard->c_task_time_ns, elapsed_ns);
    atomic_fetch_add(&shard->c_tasks_completed, 1);

    return (int)sum;
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
    
    c_task_stats_shard_t* shard = current_stats_shard();
    atomic_fetch_add(&shard->c_task_time_ns, elapsed_ns);
    atomic_fetch_add(&shard->c_tasks_completed, 1);
    
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
    
    c_task_stats_shard_t* shard = current_stats_shard();
    atomic_fetch_add(&shard->c_task_time_ns, elapsed_ns);
    atomic_fetch_add(&shard->c_tasks_completed, 1);
    
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
    
    c_task_stats_shard_t* shard = current_stats_shard();
    atomic_fetch_add(&shard->c_task_time_ns, elapsed_ns);
    atomic_fetch_add(&shard->c_tasks_completed, 1);
    
    return 0;
}

/* ============================================ */
/* Statistics                                   */
/* ============================================ */

void c_task_get_stats(c_task_stats_t* stats) {
    if (!stats) return;

    memset(stats, 0, sizeof(*stats));
    for (size_t i = 0; i < C_TASK_STATS_NUM_SHARDS; i++) {
        stats->total_c_tasks_spawned += atomic_load(&g_stats_shards[i].c_tasks_spawned);
        stats->total_c_tasks_completed += atomic_load(&g_stats_shards[i].c_tasks_completed);
        stats->total_c_task_time_ns += atomic_load(&g_stats_shards[i].c_task_time_ns);
    }
    /* total_python_task* fields are never written anywhere (dead/
     * vestigial counters) - stay zeroed, matching prior behavior. */
}

void c_task_reset_stats(void) {
    for (size_t i = 0; i < C_TASK_STATS_NUM_SHARDS; i++) {
        atomic_store(&g_stats_shards[i].c_tasks_spawned, 0);
        atomic_store(&g_stats_shards[i].c_tasks_completed, 0);
        atomic_store(&g_stats_shards[i].c_task_time_ns, 0);
    }
}
