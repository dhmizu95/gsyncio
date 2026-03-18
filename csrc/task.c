/**
 * task.c - Task/Sync model for gsyncio
 *
 * Fire-and-forget parallel work with collective synchronization
 * (like Go's goroutines with sync.WaitGroup).
 */

#include "task.h"
#include "scheduler.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* High-resolution timer */
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Global task registry */
static task_registry_t* g_task_registry = NULL;

task_registry_t* task_registry_create(void) {
    task_registry_t* reg = (task_registry_t*)calloc(1, sizeof(task_registry_t));
    if (!reg) {
        return NULL;
    }

    reg->tasks = NULL;
    reg->task_count = 0;
    reg->task_capacity = 0;
    reg->active_count = 0;
    reg->wg = waitgroup_create();
    if (!reg->wg) {
        free(reg);
        return NULL;
    }

    pthread_mutex_init(&reg->mutex, NULL);
    pthread_cond_init(&reg->cond, NULL);

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

task_handle_t* task_spawn(task_registry_t* reg, void (*func)(void*), void* arg) {
    if (!reg || !func) {
        return NULL;
    }

    task_handle_t* handle = (task_handle_t*)calloc(1, sizeof(task_handle_t));
    if (!handle) {
        return NULL;
    }

    /* Create wrapper to track completion */
    task_wrapper_arg_t* wrapper_arg = (task_wrapper_arg_t*)calloc(1, sizeof(task_wrapper_arg_t));
    if (!wrapper_arg) {
        free(handle);
        return NULL;
    }

    wrapper_arg->func = func;
    wrapper_arg->arg = arg;
    wrapper_arg->handle = handle;
    wrapper_arg->registry = reg;

    handle->state = TASK_STATE_RUNNING;
    handle->result = NULL;
    handle->exception = NULL;
    handle->wrapper_arg = wrapper_arg;

    /* Add to registry */
    pthread_mutex_lock(&reg->mutex);

    if (reg->task_count >= reg->task_capacity) {
        size_t new_capacity = reg->task_capacity == 0 ? 64 : reg->task_capacity * 2;
        task_handle_t** new_tasks = (task_handle_t**)realloc(reg->tasks, new_capacity * sizeof(task_handle_t*));
        if (!new_tasks) {
            pthread_mutex_unlock(&reg->mutex);
            free(wrapper_arg);
            free(handle);
            return NULL;
        }
        reg->tasks = new_tasks;
        reg->task_capacity = new_capacity;
    }

    reg->tasks[reg->task_count++] = handle;
    reg->active_count++;

    pthread_mutex_unlock(&reg->mutex);

    /* Increment waitgroup before spawning */
    waitgroup_add(reg->wg, 1);

    /* Spawn fiber */
    uint64_t fid = scheduler_spawn(task_wrapper, wrapper_arg);
    handle->fiber_id = fid;

    return handle;
}

void task_wrapper(void* arg) {
    task_wrapper_arg_t* wrapper = (task_wrapper_arg_t*)arg;
    if (!wrapper) {
        return;
    }

    task_registry_t* reg = wrapper->registry;
    task_handle_t* handle = wrapper->handle;

    /* Execute user function */
    wrapper->func(wrapper->arg);

    /* Mark task as completed */
    if (handle) {
        handle->state = TASK_STATE_COMPLETED;
    }

    /* Decrement waitgroup */
    if (reg) {
        waitgroup_done(reg->wg);
    }

    /* Update registry */
    if (reg) {
        pthread_mutex_lock(&reg->mutex);
        reg->active_count--;
        pthread_cond_broadcast(&reg->cond);
        pthread_mutex_unlock(&reg->mutex);
    }

    /* Free wrapper */
    free(wrapper);
}

void task_sync(task_registry_t* reg) {
    if (!reg) {
        return;
    }

    /* Wait for all tasks to complete */
    waitgroup_wait(reg->wg);
}

int task_sync_timeout(task_registry_t* reg, uint64_t timeout_ns) {
    if (!reg) {
        return -1;
    }

    uint64_t start = get_time_ns();
    uint64_t deadline = start + timeout_ns;

    pthread_mutex_lock(&reg->mutex);

    while (reg->active_count > 0) {
        uint64_t now = get_time_ns();
        if (now >= deadline) {
            pthread_mutex_unlock(&reg->mutex);
            return -1;  /* Timeout */
        }

        uint64_t remaining = deadline - now;
        struct timespec ts;
        ts.tv_sec = remaining / 1000000000ULL;
        ts.tv_nsec = remaining % 1000000000ULL;

        pthread_cond_timedwait(&reg->cond, &reg->mutex, &ts);
    }

    pthread_mutex_unlock(&reg->mutex);
    return 0;  /* All tasks completed */
}

size_t task_count(task_registry_t* reg) {
    if (!reg) {
        return 0;
    }

    pthread_mutex_lock(&reg->mutex);
    size_t count = reg->active_count;
    pthread_mutex_unlock(&reg->mutex);

    return count;
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
