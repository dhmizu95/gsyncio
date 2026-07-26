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

/* Which shard the current thread should use when no specific target
 * worker is known. Reuses the scheduler's existing worker-ID tracking
 * (same mechanism as the sharded task/completion counters), so this
 * needs no new thread-local state. */
static inline size_t current_shard_index(void) {
    return (size_t)(scheduler_get_current_worker_id() % FIBER_POOL_NUM_SHARDS);
}

static inline size_t shard_index_for(int worker_hint) {
    if (worker_hint >= 0) {
        return (size_t)worker_hint % FIBER_POOL_NUM_SHARDS;
    }
    return current_shard_index();
}

fiber_pool_t* fiber_pool_create(size_t initial_size, fiber_stack_mode_t stack_mode) {
    if (initial_size == 0) initial_size = FIBER_POOL_INITIAL_SIZE;

    fiber_pool_t* pool = (fiber_pool_t*)calloc(1, sizeof(fiber_pool_t));
    if (!pool) return NULL;

    pool->stack_mode = stack_mode;
    for (size_t s = 0; s < FIBER_POOL_NUM_SHARDS; s++) {
        pthread_spin_init(&pool->shards[s].lock, PTHREAD_PROCESS_PRIVATE);
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
        pthread_spin_lock(&shard->lock);
        fiber_t* f = (fiber_t*)atomic_load(&shard->free_list);
        while (f) {
            fiber_t* next = f->next_ready;
            if (f->stack_base) {
                munmap(f->stack_base, f->mmap_size);
            }
            free(f);
            f = next;
        }
        pthread_spin_unlock(&shard->lock);
        pthread_spin_destroy(&shard->lock);
    }
    free(pool);
}

static fiber_t* alloc_stack_for(fiber_t* f) {
#if FIBER_ALLOCATE_STACKS == 0
    /* Fibers run as plain calls on the worker's stack - see
     * FIBER_ALLOCATE_STACKS in fiber.h. Nothing reads these fields. */
    return f;
#else
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
#endif /* FIBER_ALLOCATE_STACKS */
}

/* TEMP diagnostic counters - where allocations actually land. Not exposed
 * anywhere permanent, just printed via scheduler_print_debug_info(). */
static _Atomic size_t g_diag_primary_hits = 0;
static _Atomic size_t g_diag_fallback_hits = 0;
static _Atomic size_t g_diag_grows = 0;
void fiber_pool_diag_counts(size_t* primary, size_t* fallback, size_t* grows) {
    *primary = atomic_load(&g_diag_primary_hits);
    *fallback = atomic_load(&g_diag_fallback_hits);
    *grows = atomic_load(&g_diag_grows);
}

fiber_t* fiber_pool_alloc(fiber_pool_t* pool, int worker_hint) {
    if (!pool) return NULL;

    fiber_pool_shard_t* shard = &pool->shards[shard_index_for(worker_hint)];

    pthread_spin_lock(&shard->lock);

    fiber_t* f = (fiber_t*)atomic_load(&shard->free_list);
    if (f) {
        atomic_store(&shard->free_list, f->next_ready);
        f->next_ready = NULL;
        atomic_fetch_sub(&shard->available, 1);
        pthread_spin_unlock(&shard->lock);
        atomic_fetch_add(&g_diag_primary_hits, 1);

        if (!alloc_stack_for(f)) {
            /* mmap failed - hand the fiber back to its shard and bail,
             * same recovery behavior as before sharding. */
            pthread_spin_lock(&shard->lock);
            f->next_ready = (fiber_t*)atomic_load(&shard->free_list);
            atomic_store(&shard->free_list, f);
            atomic_fetch_add(&shard->available, 1);
            pthread_spin_unlock(&shard->lock);
            return NULL;
        }

        f->state = FIBER_NEW;
        atomic_fetch_add(&pool->allocated, 1);
        return f;
    }
    pthread_spin_unlock(&shard->lock);

    /* This shard's free list is empty. Before minting a brand-new fiber
     * (which means a fresh mmap() for its stack), check whether another
     * shard is sitting on spares - fiber_pool_free() returns a fiber to
     * whichever worker actually *executed* it, not the shard it was
     * originally handed out from, so a stolen task's fiber comes back on
     * the thief's shard instead of this one. Under any real amount of
     * work-stealing that leaves this shard permanently starved while
     * fibers pile up elsewhere, even though the pool overall has plenty
     * of reusable capacity (measured: 100K tasks minted ~105K distinct
     * fibers - essentially zero reuse - before this fallback existed).
     * Scan with trylock so a contended shard is just skipped rather than
     * stalling this allocation. */
    size_t primary = shard_index_for(worker_hint);
    for (size_t i = 1; i < FIBER_POOL_NUM_SHARDS; i++) {
        fiber_pool_shard_t* other = &pool->shards[(primary + i) % FIBER_POOL_NUM_SHARDS];
        if (!atomic_load(&other->free_list)) {
            continue;
        }
        if (pthread_spin_trylock(&other->lock) != 0) {
            continue;
        }
        fiber_t* stolen = (fiber_t*)atomic_load(&other->free_list);
        if (stolen) {
            atomic_store(&other->free_list, stolen->next_ready);
            stolen->next_ready = NULL;
            atomic_fetch_sub(&other->available, 1);
        }
        pthread_spin_unlock(&other->lock);

        if (stolen) {
            if (!alloc_stack_for(stolen)) {
                pthread_spin_lock(&other->lock);
                stolen->next_ready = (fiber_t*)atomic_load(&other->free_list);
                atomic_store(&other->free_list, stolen);
                atomic_fetch_add(&other->available, 1);
                pthread_spin_unlock(&other->lock);
                return NULL;
            }
            stolen->state = FIBER_NEW;
            atomic_fetch_add(&pool->allocated, 1);
            atomic_fetch_add(&g_diag_fallback_hits, 1);
            return stolen;
        }
    }
    atomic_fetch_add(&g_diag_grows, 1);

    /* Nobody has a spare - grow. Growth is a single lock-free counter
     * shared by all shards, so this never contends with any shard's
     * lock. */
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
    pthread_spin_lock(&shard->lock);
    fiber->next_ready = (fiber_t*)atomic_load(&shard->free_list);
    atomic_store(&shard->free_list, fiber);
    atomic_fetch_add(&shard->available, 1);
    pthread_spin_unlock(&shard->lock);

    atomic_fetch_sub(&pool->allocated, 1);
}

size_t fiber_pool_capacity(fiber_pool_t* pool) {
    return pool ? atomic_load(&pool->capacity) : 0;
}

