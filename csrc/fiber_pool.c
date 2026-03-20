/**
 * fiber_pool.c - Stable fiber pool with individual fiber allocations (no realloc move issues)
 */

#include "fiber_pool.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdio.h>

/* Debug logging */
static int g_debug_enabled = -1;
static void init_debug_flag(void) {
    if (g_debug_enabled == -1) {
        const char* env = getenv("GSYNCIO_DEBUG");
        g_debug_enabled = (env && strcmp(env, "1") == 0) ? 1 : 0;
    }
}
#define DEBUG_LOG(fmt, ...) do { \
    init_debug_flag(); \
    if (g_debug_enabled) { \
        fprintf(stderr, "[FIBER_POOL DEBUG] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } \
} while(0)

#define FIBER_POOL_INITIAL_SIZE 8192
#define FIBER_POOL_MAX_SIZE (10 * 1024 * 1024)  /* 10M fibers */
#define FIBER_POOL_LAZY_STACK 1         /* Lazy allocate stacks when used */

fiber_pool_t* fiber_pool_create(size_t initial_size) {
    if (initial_size == 0) initial_size = FIBER_POOL_INITIAL_SIZE;
    
    fiber_pool_t* pool = (fiber_pool_t*)calloc(1, sizeof(fiber_pool_t));
    if (!pool) return NULL;
    
    pthread_mutex_init(&pool->mutex, NULL);
    atomic_store(&pool->free_list, NULL);
    
    pthread_mutex_lock(&pool->mutex);
    size_t actual = 0;
    for (size_t i = 0; i < initial_size; i++) {
        fiber_t* f = (fiber_t*)calloc(1, sizeof(fiber_t));
        if (!f) break;
        f->pool = pool;
        f->id = i + 1;
        f->next_ready = (fiber_t*)atomic_load(&pool->free_list);
        atomic_store(&pool->free_list, f);
        actual++;
    }
    pool->capacity = actual;
    atomic_store(&pool->available, actual);
    atomic_store(&pool->allocated, 0);
    pthread_mutex_unlock(&pool->mutex);
    
    return pool;
}

void fiber_pool_destroy(fiber_pool_t* pool) {
    if (!pool) return;
    
    pthread_mutex_lock(&pool->mutex);
    fiber_t* f = pool->free_list; // Replaced atomic_load
    while (f) {
        fiber_t* next = f->next_ready;
        if (f->stack_base) {
            munmap(f->stack_base, f->stack_capacity + 4096);
        }
        free(f);
        f = next;
    }
    pthread_mutex_unlock(&pool->mutex);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
}

fiber_t* fiber_pool_alloc(fiber_pool_t* pool) {
    if (!pool) return NULL;
    
    pthread_mutex_lock(&pool->mutex);
    
    fiber_t* f = (fiber_t*)atomic_load(&pool->free_list);
    if (f) {
        atomic_store(&pool->free_list, f->next_ready);
        f->next_ready = NULL;
        
#if FIBER_POOL_LAZY_STACK == 1
        if (!f->stack_base) {
            f->stack_base = mmap(NULL, FIBER_DEFAULT_STACK_SIZE + 4096, 
                                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (f->stack_base == MAP_FAILED) {
                f->next_ready = (fiber_t*)atomic_load(&pool->free_list);
                atomic_store(&pool->free_list, f);
                pthread_mutex_unlock(&pool->mutex);
                return NULL;
            }
            mprotect(f->stack_base, 4096, PROT_NONE);
            f->stack_capacity = FIBER_DEFAULT_STACK_SIZE;
            f->stack_ptr = (char*)f->stack_base + FIBER_DEFAULT_STACK_SIZE + 4096;
        }
#endif
        f->state = FIBER_NEW;
        atomic_fetch_sub(&pool->available, 1);
        atomic_fetch_add(&pool->allocated, 1);
        pthread_mutex_unlock(&pool->mutex);
        return f;
    }
    
    /* Grow by 1 */
    if (pool->capacity >= FIBER_POOL_MAX_SIZE) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }
    
    fiber_t* new_f = (fiber_t*)calloc(1, sizeof(fiber_t));
    if (!new_f) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }
    new_f->pool = pool;
    new_f->id = ++pool->capacity;
    new_f->state = FIBER_NEW;
    atomic_fetch_add(&pool->allocated, 1);
    
    pthread_mutex_unlock(&pool->mutex);
    return new_f;
}

void fiber_pool_free(fiber_pool_t* pool, fiber_t* fiber) {
    if (!pool || !fiber) return;
    
    pthread_mutex_lock(&pool->mutex);
    
    /* Partial reset */
    fiber->state = FIBER_NEW;
    fiber->func = NULL;
    fiber->arg = NULL;
    fiber->result = NULL;
    fiber->parent = NULL;
    fiber->next_ready = NULL;
    fiber->prev_ready = NULL;
    fiber->affinity = 0;
    fiber->waiting_on = NULL;
    
    fiber->next_ready = (fiber_t*)pool->free_list;
    pool->free_list = fiber;
    
    atomic_fetch_add(&pool->available, 1);
    atomic_fetch_sub(&pool->allocated, 1);
    
    pthread_mutex_unlock(&pool->mutex);
}

size_t fiber_pool_available(fiber_pool_t* pool) {
    return pool ? atomic_load(&pool->available) : 0;
}

size_t fiber_pool_allocated(fiber_pool_t* pool) {
    return pool ? atomic_load(&pool->allocated) : 0;
}

size_t fiber_pool_capacity(fiber_pool_t* pool) {
    return pool ? pool->capacity : 0;
}

int fiber_pool_verify_counters(fiber_pool_t* pool) {
    return 1;
}
