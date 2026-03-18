/**
 * task.c - Task/Sync model for gsyncio
 *
 * Fire-and-forget parallel work with collective synchronization.
 * Optimized C implementation with batch spawning and lock-free tracking.
 */

#include "task.h"
#include "scheduler.h"
#include "fiber.h"
#include "fiber_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/mman.h>

/* High-resolution timer */
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Global task registry */
static task_registry_t* g_task_registry = NULL;

/* ============================================ */
/* Task Registry Implementation                */
/* ============================================ */

task_registry_t* task_registry_create(void) {
    task_registry_t* reg = (task_registry_t*)calloc(1, sizeof(task_registry_t));
    if (!reg) {
        return NULL;
    }

    reg->tasks = NULL;
    reg->task_count = 0;
    reg->task_capacity = 0;
    atomic_store(&reg->active_count, 0);
    atomic_store(&reg->completion_count, 0);
    
    reg->wg = waitgroup_create();
    if (!reg->wg) {
        free(reg);
        return NULL;
    }

    /* Initialize semaphore-like condition */
    pthread_mutex_init(&reg->mutex, NULL);
    pthread_cond_init(&reg->cond, NULL);
    reg->waiters = 0;

    return reg;
}

void task_registry_destroy(task_registry_t* reg) {
    if (!reg) {
        return;
    }

    if (reg->tasks) {
        free(reg->tasks);
    }

    if (reg->wg) {
        waitgroup_destroy(reg->wg);
    }

    pthread_mutex_destroy(&reg->mutex);
    pthread_cond_destroy(&reg->cond);

    free(reg);
}

/* Internal task wrapper that tracks completion */
typedef struct task_wrapper_arg {
    void (*func)(void*);
    void* arg;
    task_registry_t* registry;
    uint64_t task_id;
} task_wrapper_arg_t;

void task_wrapper(void* arg) {
    task_wrapper_arg_t* wrapper = (task_wrapper_arg_t*)arg;
    if (!wrapper) {
        return;
    }

    /* Execute user function */
    wrapper->func(wrapper->arg);

    /* Mark task as completed - atomic increment */
    if (wrapper->registry) {
        task_registry_t* reg = wrapper->registry;
        
        /* Atomic increment of completion count */
        uint64_t completed = atomic_fetch_add(&reg->completion_count, 1) + 1;
        
        /* Atomic decrement of active count */
        uint64_t active = atomic_fetch_sub(&reg->active_count, 1) - 1;
        
        /* If last task, wake up waiters */
        if (active == 0) {
            pthread_mutex_lock(&reg->mutex);
            reg->all_done = 1;
            pthread_cond_broadcast(&reg->cond);
            pthread_mutex_unlock(&reg->mutex);
        }
    }

    /* Free wrapper */
    free(wrapper);
}

task_handle_t* task_spawn(task_registry_t* reg, void (*func)(void*), void* arg) {
    if (!reg || !func) {
        return NULL;
    }

    /* Create wrapper */
    task_wrapper_arg_t* wrapper = (task_wrapper_arg_t*)calloc(1, sizeof(task_wrapper_arg_t));
    if (!wrapper) {
        return NULL;
    }

    wrapper->func = func;
    wrapper->arg = arg;
    wrapper->registry = reg;
    wrapper->task_id = atomic_fetch_add(&reg->task_count, 1);

    /* Create handle */
    task_handle_t* handle = (task_handle_t*)calloc(1, sizeof(task_handle_t));
    if (!handle) {
        free(wrapper);
        return NULL;
    }

    handle->task_id = wrapper->task_id;
    handle->state = TASK_STATE_RUNNING;
    handle->wrapper_arg = wrapper;

    /* Atomic increment of active count BEFORE spawning */
    atomic_fetch_add(&reg->active_count, 1);

    /* Spawn fiber */
    uint64_t fid = scheduler_spawn(task_wrapper, wrapper);
    handle->fiber_id = fid;

    if (fid == 0) {
        /* Spawn failed */
        atomic_fetch_sub(&reg->active_count, 1);
        free(wrapper);
        free(handle);
        return NULL;
    }

    return handle;
}

/* ============================================ */
/* Batch Task Spawning - Key Optimization     */
/* ============================================ */

task_batch_t* task_batch_create(size_t capacity) {
    if (capacity == 0) {
        capacity = 64;
    }

    task_batch_t* batch = (task_batch_t*)calloc(1, sizeof(task_batch_t));
    if (!batch) {
        return NULL;
    }

    batch->funcs = (void(**)(void*))calloc(capacity, sizeof(void(*)(void*)));
    batch->args = (void**)calloc(capacity, sizeof(void*));
    batch->handles = (task_handle_t**)calloc(capacity, sizeof(task_handle_t*));
    
    if (!batch->funcs || !batch->args || !batch->handles) {
        free(batch->funcs);
        free(batch->args);
        free(batch->handles);
        free(batch);
        return NULL;
    }

    batch->capacity = capacity;
    batch->count = 0;
    batch->registry = NULL;

    return batch;
}

void task_batch_destroy(task_batch_t* batch) {
    if (!batch) {
        return;
    }

    /* Free any pending handles */
    for (size_t i = 0; i < batch->count; i++) {
        if (batch->handles[i]) {
            free(batch->handles[i]);
        }
    }

    free(batch->funcs);
    free(batch->args);
    free(batch->handles);
    free(batch);
}

int task_batch_add(task_batch_t* batch, void (*func)(void*), void* arg) {
    if (!batch || !func) {
        return -1;
    }

    /* Grow if needed */
    if (batch->count >= batch->capacity) {
        size_t new_capacity = batch->capacity * 2;
        
        void (**new_funcs)(void*) = realloc(batch->funcs, new_capacity * sizeof(void(*)(void*)));
        void** new_args = realloc(batch->args, new_capacity * sizeof(void*));
        task_handle_t** new_handles = realloc(batch->handles, new_capacity * sizeof(task_handle_t*));
        
        if (!new_funcs || !new_args || !new_handles) {
            return -1;
        }
        
        batch->funcs = new_funcs;
        batch->args = new_args;
        batch->handles = new_handles;
        batch->capacity = new_capacity;
    }

    batch->funcs[batch->count] = func;
    batch->args[batch->count] = arg;
    batch->handles[batch->count] = NULL;
    batch->count++;

    return 0;
}

/* Spawn all tasks in batch with minimal overhead */
int task_batch_spawn(task_batch_t* batch, task_registry_t* reg) {
    if (!batch || !reg || batch->count == 0) {
        return -1;
    }

    batch->registry = reg;

    /* Pre-increment active count for all tasks (single atomic operation) */
    atomic_fetch_add(&reg->active_count, batch->count);

    /* Spawn all fibers */
    for (size_t i = 0; i < batch->count; i++) {
        /* Create wrapper */
        task_wrapper_arg_t* wrapper = (task_wrapper_arg_t*)calloc(1, sizeof(task_wrapper_arg_t));
        if (!wrapper) {
            /* Rollback on failure */
            atomic_fetch_sub(&reg->active_count, batch->count - i);
            return -1;
        }

        wrapper->func = batch->funcs[i];
        wrapper->arg = batch->args[i];
        wrapper->registry = reg;
        wrapper->task_id = atomic_fetch_add(&reg->task_count, 1);

        /* Create handle */
        task_handle_t* handle = (task_handle_t*)calloc(1, sizeof(task_handle_t));
        if (!handle) {
            free(wrapper);
            atomic_fetch_sub(&reg->active_count, batch->count - i);
            return -1;
        }

        handle->task_id = wrapper->task_id;
        handle->state = TASK_STATE_RUNNING;
        handle->wrapper_arg = wrapper;

        /* Spawn fiber */
        uint64_t fid = scheduler_spawn(task_wrapper, wrapper);
        handle->fiber_id = fid;

        if (fid == 0) {
            atomic_fetch_sub(&reg->active_count, 1);
            free(wrapper);
            free(handle);
            return -1;
        }

        batch->handles[i] = handle;
    }

    return 0;
}

/* ============================================ */
/* Synchronization                             */
/* ============================================ */

void task_sync(task_registry_t* reg) {
    if (!reg) {
        return;
    }

    /* Wait using condition variable */
    pthread_mutex_lock(&reg->mutex);

    /* Check inside mutex to avoid race condition */
    if (atomic_load(&reg->active_count) == 0) {
        pthread_mutex_unlock(&reg->mutex);
        return;
    }

    /* Reset all_done flag for this wait cycle */
    reg->all_done = 0;

    while (atomic_load(&reg->active_count) > 0) {
        reg->waiters++;
        pthread_cond_wait(&reg->cond, &reg->mutex);
        reg->waiters--;
    }

    pthread_mutex_unlock(&reg->mutex);
}

int task_sync_timeout(task_registry_t* reg, uint64_t timeout_ns) {
    if (!reg) {
        return -1;
    }

    uint64_t deadline = get_time_ns() + timeout_ns;

    pthread_mutex_lock(&reg->mutex);

    /* Check inside mutex to avoid race condition */
    if (atomic_load(&reg->active_count) == 0) {
        pthread_mutex_unlock(&reg->mutex);
        return 0;
    }

    /* Reset all_done flag for this wait cycle */
    reg->all_done = 0;

    while (atomic_load(&reg->active_count) > 0) {
        uint64_t now = get_time_ns();
        if (now >= deadline) {
            pthread_mutex_unlock(&reg->mutex);
            return -1;  /* Timeout */
        }

        uint64_t remaining = deadline - now;
        struct timespec ts;
        ts.tv_sec = remaining / 1000000000ULL;
        ts.tv_nsec = remaining % 1000000000ULL;

        reg->waiters++;
        int ret = pthread_cond_timedwait(&reg->cond, &reg->mutex, &ts);
        reg->waiters--;

        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&reg->mutex);
            return -1;
        }
    }

    pthread_mutex_unlock(&reg->mutex);
    return 0;  /* All tasks completed */
}

size_t task_count(task_registry_t* reg) {
    if (!reg) {
        return 0;
    }
    return atomic_load(&reg->active_count);
}

size_t task_completed_count(task_registry_t* reg) {
    if (!reg) {
        return 0;
    }
    return atomic_load(&reg->completion_count);
}

task_registry_t* task_get_registry(void) {
    return g_task_registry;
}

void task_set_registry(task_registry_t* reg) {
    if (g_task_registry && g_task_registry != reg) {
        task_registry_destroy(g_task_registry);
    }
    g_task_registry = reg;
}

void task_reset_registry(task_registry_t* reg) {
    if (!reg) {
        return;
    }

    atomic_store(&reg->active_count, 0);
    atomic_store(&reg->completion_count, 0);
    atomic_store(&reg->task_count, 0);
    reg->all_done = 0;
}

/* ============================================ */
/* Ultra-Fast Batch Spawn Implementation        */
/* ============================================ */

task_batch_fast_t* task_batch_fast_create(size_t capacity) {
    if (capacity == 0) {
        capacity = 256;  /* Default larger capacity for batch ops */
    }

    task_batch_fast_t* batch = (task_batch_fast_t*)calloc(1, sizeof(task_batch_fast_t));
    if (!batch) {
        return NULL;
    }

    batch->funcs = (void (**)(void*))calloc(capacity, sizeof(void(*)(void*)));
    batch->args = (void**)calloc(capacity, sizeof(void*));
    batch->fiber_ids = (uint64_t*)calloc(capacity, sizeof(uint64_t));

    if (!batch->funcs || !batch->args || !batch->fiber_ids) {
        free(batch->funcs);
        free(batch->args);
        free(batch->fiber_ids);
        free(batch);
        return NULL;
    }

    batch->capacity = capacity;
    batch->count = 0;
    batch->store_fiber_ids = 0;

    return batch;
}

void task_batch_fast_destroy(task_batch_fast_t* batch, int free_args) {
    if (!batch) {
        return;
    }

    /* Optionally free argument storage (caller manages Python refs) */
    if (free_args && batch->args) {
        /* Args are Python objects - don't free here, caller manages */
        /* Just clear the array */
        memset(batch->args, 0, batch->capacity * sizeof(void*));
    }

    free(batch->funcs);
    free(batch->args);
    free(batch->fiber_ids);
    free(batch);
}

int task_batch_fast_add(task_batch_fast_t* batch, void (*func)(void*), void* arg) {
    if (!batch || !func) {
        return -1;
    }

    /* Grow if needed */
    if (batch->count >= batch->capacity) {
        size_t new_capacity = batch->capacity * 2;

        void (**new_funcs)(void*) = realloc(batch->funcs, new_capacity * sizeof(void(*)(void*)));
        void** new_args = realloc(batch->args, new_capacity * sizeof(void*));
        uint64_t* new_ids = realloc(batch->fiber_ids, new_capacity * sizeof(uint64_t));

        if (!new_funcs || !new_args || !new_ids) {
            return -1;
        }

        batch->funcs = new_funcs;
        batch->args = new_args;
        batch->fiber_ids = new_ids;
        batch->capacity = new_capacity;
    }

    batch->funcs[batch->count] = func;
    batch->args[batch->count] = arg;
    batch->fiber_ids[batch->count] = 0;
    batch->count++;

    return 0;
}

/* Forward declaration from scheduler */
extern scheduler_t* g_scheduler;
uint64_t scheduler_spawn(void (*entry)(void*), void* user_data);

/**
 * Ultra-fast batch spawn - runs completely without GIL
 * 
 * Key optimizations:
 * 1. All function pointers and args pre-stored in C arrays
 * 2. Single atomic increment for active_count (batch operation)
 * 3. No Python API calls during spawn loop
 * 4. Minimal error checking (assumes valid input)
 * 5. Direct fiber allocation from pool
 */
size_t task_batch_fast_spawn_nogil(task_batch_fast_t* batch, task_registry_t* reg) {
    if (!batch || !reg || batch->count == 0 || !g_scheduler) {
        return 0;
    }

    size_t spawned = 0;
    
    /* Pre-increment active count for ALL tasks in one atomic op */
    atomic_fetch_add(&reg->active_count, batch->count);

    /* Spawn all fibers - NO GIL needed here */
    for (size_t i = 0; i < batch->count; i++) {
        /* Create minimal wrapper */
        task_wrapper_arg_t* wrapper = (task_wrapper_arg_t*)calloc(1, sizeof(task_wrapper_arg_t));
        if (!wrapper) {
            /* Continue on failure - best effort batch spawn */
            atomic_fetch_sub(&reg->active_count, 1);
            continue;
        }

        wrapper->func = batch->funcs[i];
        wrapper->arg = batch->args[i];
        wrapper->registry = reg;
        wrapper->task_id = atomic_fetch_add(&reg->task_count, 1);

        /* Try fiber pool first (fast path) */
        fiber_t* f = NULL;
        if (g_scheduler->fiber_pool) {
            f = fiber_pool_alloc((fiber_pool_t*)g_scheduler->fiber_pool);
        }

        /* Fall back to direct allocation */
        if (!f) {
            f = fiber_create(batch->funcs[i], wrapper, g_scheduler->config.stack_size);
        } else {
            /* Initialize pooled fiber */
            f->func = batch->funcs[i];
            f->arg = wrapper;
            f->parent = fiber_current();

            /* Lazy stack allocation */
            if (!f->stack_base) {
                size_t stack_size = g_scheduler->config.stack_size > 0 ?
                    g_scheduler->config.stack_size : FIBER_DEFAULT_STACK_SIZE;
                f->stack_base = mmap(
                    NULL,
                    stack_size + 4096,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1,
                    0
                );
                if (f->stack_base == MAP_FAILED) {
                    fiber_pool_free((fiber_pool_t*)g_scheduler->fiber_pool, f);
                    atomic_fetch_sub(&reg->active_count, 1);
                    free(wrapper);
                    continue;
                }
                mprotect(f->stack_base, 4096, PROT_NONE);
                f->stack_size = stack_size;
                f->stack_capacity = stack_size;
                f->stack_ptr = (char*)f->stack_base + stack_size + 4096;
            }
        }

        if (!f) {
            atomic_fetch_sub(&reg->active_count, 1);
            free(wrapper);
            continue;
        }

        g_scheduler->stats.total_fibers_created++;

        /* Schedule fiber */
        int worker_id = g_scheduler->next_worker % g_scheduler->num_workers;
        g_scheduler->next_worker++;
        scheduler_schedule(f, worker_id);

        /* Store fiber ID if requested */
        if (batch->store_fiber_ids) {
            batch->fiber_ids[i] = fiber_id(f);
        }

        spawned++;
    }

    return spawned;
}
