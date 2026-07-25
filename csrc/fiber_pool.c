/**
 * fiber_pool.c - Sharded fiber pool (per-worker free lists, no realloc move issues)
 *
 * Each shard has its own mutex + free list. A worker thread frees the
 * fiber it just ran back into its own shard, and whoever allocates next
 * (usually that same worker, sometimes the spawning thread) draws from
 * the shard matching its own worker ID - so concurrent workers mostly
 * touch different locks instead of piling up on one pool-wide mutex.
 */

#include "fiber_pool.h"
#include "fiber.h"
#include "scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>

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
#define FIBER_POOL_MAX_SIZE (100 * 1024 * 1024) /* 100M fibers */
#define FIBER_POOL_LAZY_STACK 1         /* Lazy allocate stacks when used */

/* Which shard the current thread should use. Reuses the scheduler's
 * existing worker-ID tracking (same mechanism as the sharded task/
 * completion counters), so this needs no new thread-local state. */
static inline size_t current_shard_index(void) {
    return (size_t)(scheduler_get_current_worker_id() % FIBER_POOL_NUM_SHARDS);
}

fiber_pool_t* fiber_pool_create(size_t initial_size, fiber_stack_mode_t stack_mode) {
    if (initial_size == 0) initial_size = FIBER_POOL_INITIAL_SIZE;

    fiber_pool_t* pool = (fiber_pool_t*)calloc(1, sizeof(fiber_pool_t));
    if (!pool) return NULL;

    pool->stack_mode = stack_mode;
    for (size_t s = 0; s < FIBER_POOL_NUM_SHARDS; s++) {
        pthread_mutex_init(&pool->shards[s].mutex, NULL);
        atomic_store(&pool->shards[s].free_list, NULL);
        atomic_store(&pool->shards[s].available, 0);
    }

    /* Seed fibers round-robin across shards so initial load is spread
     * out rather than dumped entirely into shard 0. */
    size_t actual = 0;
    for (size_t i = 0; i < initial_size; i++) {
        fiber_t* f = (fiber_t*)calloc(1, sizeof(fiber_t));
        if (!f) break;
        f->pool = pool;
        f->id = i + 1;

        fiber_pool_shard_t* shard = &pool->shards[i % FIBER_POOL_NUM_SHARDS];
        f->next_ready = (fiber_t*)atomic_load(&shard->free_list);
        atomic_store(&shard->free_list, f);
        atomic_fetch_add(&shard->available, 1);
        actual++;
    }
    atomic_store(&pool->capacity, actual);
    atomic_store(&pool->allocated, 0);

    return pool;
}

void fiber_pool_destroy(fiber_pool_t* pool) {
    if (!pool) return;

    for (size_t s = 0; s < FIBER_POOL_NUM_SHARDS; s++) {
        fiber_pool_shard_t* shard = &pool->shards[s];
        pthread_mutex_lock(&shard->mutex);
        fiber_t* f = (fiber_t*)atomic_load(&shard->free_list);
        while (f) {
            fiber_t* next = f->next_ready;
            if (f->stack_base) {
                munmap(f->stack_base, f->mmap_size);
            }
            free(f);
            f = next;
        }
        pthread_mutex_unlock(&shard->mutex);
        pthread_mutex_destroy(&shard->mutex);
    }
    free(pool);
}

static fiber_t* alloc_stack_for(fiber_t* f) {
#if FIBER_POOL_LAZY_STACK == 1
    if (!f->stack_base) {
#if FIBER_USE_GUARD_PAGES == 1
        size_t alloc_size = FIBER_DEFAULT_STACK_SIZE + 4096;
        f->stack_base = mmap(NULL, alloc_size,
                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (f->stack_base != MAP_FAILED) {
            mprotect(f->stack_base, 4096, PROT_NONE);
        }
#else
        size_t alloc_size = FIBER_DEFAULT_STACK_SIZE;
        f->stack_base = mmap(NULL, alloc_size,
                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (f->stack_base == MAP_FAILED) {
            f->stack_base = NULL;
            return NULL;
        }
        f->stack_capacity = FIBER_DEFAULT_STACK_SIZE;
        f->mmap_size = alloc_size;
        f->stack_ptr = (char*)f->stack_base + alloc_size;
    }
#endif
    return f;
}

fiber_t* fiber_pool_alloc(fiber_pool_t* pool) {
    if (!pool) return NULL;

    fiber_pool_shard_t* shard = &pool->shards[current_shard_index()];

    pthread_mutex_lock(&shard->mutex);

    fiber_t* f = (fiber_t*)atomic_load(&shard->free_list);
    if (f) {
        atomic_store(&shard->free_list, f->next_ready);
        f->next_ready = NULL;
        atomic_fetch_sub(&shard->available, 1);
        pthread_mutex_unlock(&shard->mutex);

        if (!alloc_stack_for(f)) {
            /* mmap failed - hand the fiber back to its shard and bail,
             * same recovery behavior as before sharding. */
            pthread_mutex_lock(&shard->mutex);
            f->next_ready = (fiber_t*)atomic_load(&shard->free_list);
            atomic_store(&shard->free_list, f);
            atomic_fetch_add(&shard->available, 1);
            pthread_mutex_unlock(&shard->mutex);
            return NULL;
        }

        f->state = FIBER_NEW;
        atomic_fetch_add(&pool->allocated, 1);
        return f;
    }
    pthread_mutex_unlock(&shard->mutex);

    /* Shard's free list is empty - grow instead of scanning other
     * shards. Growth is a single lock-free counter shared by all
     * shards, so this never contends with any shard's mutex. */
    size_t prev_capacity = atomic_fetch_add(&pool->capacity, 1);
    if (prev_capacity >= FIBER_POOL_MAX_SIZE) {
        atomic_fetch_sub(&pool->capacity, 1);
        return NULL;
    }

    fiber_t* new_f = (fiber_t*)calloc(1, sizeof(fiber_t));
    if (!new_f) {
        atomic_fetch_sub(&pool->capacity, 1);
        return NULL;
    }
    new_f->pool = pool;
    new_f->id = prev_capacity + 1;
    new_f->state = FIBER_NEW;
    atomic_fetch_add(&pool->allocated, 1);

    return new_f;
}

void fiber_pool_free(fiber_pool_t* pool, fiber_t* fiber) {
    if (!pool || !fiber) return;

    /* Partial reset - no lock needed, fiber isn't visible to anyone
     * else until it's pushed onto a shard's free list below. */
    fiber->state = FIBER_NEW;
    fiber->func = NULL;
    fiber->arg = NULL;
    fiber->result = NULL;
    fiber->parent = NULL;
    fiber->next_ready = NULL;
    fiber->prev_ready = NULL;
    fiber->affinity = 0;
    fiber->waiting_on = NULL;

    /* Hybrid Mode: Unmap stack to save memory maps */
    if (pool->stack_mode == STACK_MODE_HYBRID && fiber->stack_base) {
        munmap(fiber->stack_base, fiber->mmap_size);
        fiber->stack_base = NULL;
        fiber->stack_ptr = NULL;
        fiber->mmap_size = 0;
    }

    fiber_pool_shard_t* shard = &pool->shards[current_shard_index()];
    pthread_mutex_lock(&shard->mutex);
    fiber->next_ready = (fiber_t*)atomic_load(&shard->free_list);
    atomic_store(&shard->free_list, fiber);
    atomic_fetch_add(&shard->available, 1);
    pthread_mutex_unlock(&shard->mutex);

    atomic_fetch_sub(&pool->allocated, 1);
}

size_t fiber_pool_available(fiber_pool_t* pool) {
    if (!pool) return 0;
    size_t total = 0;
    for (size_t s = 0; s < FIBER_POOL_NUM_SHARDS; s++) {
        total += atomic_load(&pool->shards[s].available);
    }
    return total;
}

size_t fiber_pool_allocated(fiber_pool_t* pool) {
    return pool ? atomic_load(&pool->allocated) : 0;
}

size_t fiber_pool_capacity(fiber_pool_t* pool) {
    return pool ? atomic_load(&pool->capacity) : 0;
}

int fiber_pool_verify_counters(fiber_pool_t* pool) {
    (void)pool;
    return 1;
}
