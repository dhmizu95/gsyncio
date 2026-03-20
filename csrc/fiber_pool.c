/**
 * fiber_pool.c - Fiber pool implementation for gsyncio
 * 
 * Object pool for efficient fiber allocation.
 * Pre-allocates fiber control blocks and stacks to reduce allocation overhead.
 * Uses lock-free stack for O(1) allocation/deallocation.
 */

#include "fiber_pool.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>

/* ============================================ */
/* Configuration                               */
/* ============================================ */

#define FIBER_POOL_INITIAL_SIZE 65536      /* Pre-allocate 64K fibers */
#define FIBER_POOL_GROWTH_FACTOR 2
#define FIBER_POOL_MAX_SIZE (10 * 1024 * 1024)  /* 10M fibers */
#define FIBER_POOL_LAZY_STACK 1            /* Lazy stack allocation for memory efficiency */

/* Lock-free free list node */
typedef struct free_node {
    fiber_t* fiber;
    _Atomic(struct free_node*) next;
} free_node_t;

/* ============================================ */
/* Fiber Pool Implementation                   */
/* ============================================ */

fiber_pool_t* fiber_pool_create(size_t initial_size) {
    if (initial_size == 0) {
        initial_size = FIBER_POOL_INITIAL_SIZE;
    }
    if (initial_size > FIBER_POOL_MAX_SIZE) {
        initial_size = FIBER_POOL_MAX_SIZE;
    }
    
    fiber_pool_t* pool = (fiber_pool_t*)calloc(1, sizeof(fiber_pool_t));
    if (!pool) {
        return NULL;
    }
    
    pool->capacity = initial_size;
    
    /* Pre-allocate fiber control blocks */
    pool->fibers = (fiber_t*)calloc(pool->capacity, sizeof(fiber_t));
    if (!pool->fibers) {
        free(pool);
        return NULL;
    }
    
    /* Pre-allocate lock-free free list nodes */
    free_node_t* nodes = (free_node_t*)calloc(pool->capacity, sizeof(free_node_t));
    if (!nodes) {
        free(pool->fibers);
        free(pool);
        return NULL;
    }
    
    /* Initialize lock-free free list */
    atomic_store(&pool->free_list, NULL);
    
    /* Pre-allocate stacks for all fibers and push to free list */
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->fibers[i].id = i;
        pool->fibers[i].pool = pool;
#if FIBER_POOL_LAZY_STACK == 0
        pool->fibers[i].stack_base = mmap(
            NULL,
            FIBER_DEFAULT_STACK_SIZE + 4096,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        );
        if (pool->fibers[i].stack_base != MAP_FAILED) {
            mprotect(pool->fibers[i].stack_base, 4096, PROT_NONE);
            pool->fibers[i].stack_capacity = FIBER_DEFAULT_STACK_SIZE;
            pool->fibers[i].stack_ptr = (char*)pool->fibers[i].stack_base + FIBER_DEFAULT_STACK_SIZE + 4096;
        }
#endif
        /* Push to lock-free free list */
        nodes[i].fiber = &pool->fibers[i];
        atomic_store(&nodes[i].next, atomic_load(&pool->free_list));
        atomic_store(&pool->free_list, &nodes[i]);
    }
    
    /* Store nodes array for cleanup */
    pool->_nodes = (void*)nodes;
    
    pool->available = pool->capacity;
    pool->allocated = 0;
    
    pthread_mutex_init(&pool->mutex, NULL);
    
    return pool;
}

void fiber_pool_destroy(fiber_pool_t* pool) {
    if (!pool) {
        return;
    }
    
    pthread_mutex_destroy(&pool->mutex);
    
    /* Free stacks */
#if FIBER_POOL_LAZY_STACK == 0
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->fibers[i].stack_base != NULL && pool->fibers[i].stack_base != MAP_FAILED) {
            munmap(pool->fibers[i].stack_base, pool->fibers[i].stack_capacity + 4096);
        }
    }
#endif
    
    free(pool->_nodes);
    free(pool->fibers);
    free(pool);
}

/* Lock-free allocation using atomic CAS */
fiber_t* fiber_pool_alloc(fiber_pool_t* pool) {
    if (!pool) {
        return NULL;
    }
    
    /* Fast path: lock-free pop from free list */
    free_node_t* head = atomic_load(&pool->free_list);
    
    while (head != NULL) {
        free_node_t* next = atomic_load(&head->next);
        if (atomic_compare_exchange_weak(&pool->free_list, &head, next)) {
            /* Successfully popped */
            fiber_t* fiber = head->fiber;
            
            /* Reset fiber state (keep pre-allocated stack) */
            void* saved_stack_base = fiber->stack_base;
            size_t saved_stack_capacity = fiber->stack_capacity;
            char* saved_stack_ptr = fiber->stack_ptr;
            
            memset(fiber, 0, sizeof(fiber_t));
            
            /* Restore pre-allocated stack */
            fiber->stack_base = saved_stack_base;
            fiber->stack_capacity = saved_stack_capacity;
            fiber->stack_ptr = saved_stack_ptr;
            
            fiber->id = atomic_fetch_add(&pool->allocated, 1) + 1;  /* Unique ID */
            fiber->pool = pool;
            fiber->state = FIBER_NEW;
            
            /* Note: Not adding to fiber table - pool fibers are tracked separately */
            
            atomic_fetch_sub(&pool->available, 1);
            
            return fiber;
        }
        /* CAS failed, retry with new head */
        head = atomic_load(&pool->free_list);
    }
    
    /* Slow path: need to grow pool (requires mutex) */
    pthread_mutex_lock(&pool->mutex);
    
    /* Double-check after acquiring lock */
    head = atomic_load(&pool->free_list);
    if (head != NULL) {
        pthread_mutex_unlock(&pool->mutex);
        return fiber_pool_alloc(pool);  /* Retry fast path */
    }
    
    /* Grow the pool */
    size_t old_capacity = pool->capacity;
    size_t new_size = old_capacity * FIBER_POOL_GROWTH_FACTOR;
    if (new_size > FIBER_POOL_MAX_SIZE) {
        new_size = FIBER_POOL_MAX_SIZE;
    }
    if (new_size == old_capacity) {
        /* At max capacity */
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }
    
    /* Reallocate fiber array */
    fiber_t* new_fibers = (fiber_t*)realloc(pool->fibers, new_size * sizeof(fiber_t));
    if (!new_fibers) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }
    pool->fibers = new_fibers;
    
    /* Reallocate nodes array */
    free_node_t* new_nodes = (free_node_t*)realloc(pool->_nodes, new_size * sizeof(free_node_t));
    if (!new_nodes) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }
    pool->_nodes = (void*)new_nodes;
    
    /* Initialize new fibers and add to free list */
    size_t grow_count = new_size - old_capacity;
    for (size_t i = 0; i < grow_count; i++) {
        size_t idx = old_capacity + i;
        pool->fibers[idx].id = idx;
        pool->fibers[idx].pool = pool;
#if FIBER_POOL_LAZY_STACK == 0
        pool->fibers[idx].stack_base = mmap(
            NULL,
            FIBER_DEFAULT_STACK_SIZE + 4096,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        );
        if (pool->fibers[idx].stack_base != MAP_FAILED) {
            mprotect(pool->fibers[idx].stack_base, 4096, PROT_NONE);
            pool->fibers[idx].stack_capacity = FIBER_DEFAULT_STACK_SIZE;
            pool->fibers[idx].stack_ptr = (char*)pool->fibers[idx].stack_base + FIBER_DEFAULT_STACK_SIZE + 4096;
        }
#endif
        /* Add to lock-free free list (push) */
        free_node_t* node = &new_nodes[idx];
        node->fiber = &pool->fibers[idx];
        atomic_store(&node->next, atomic_load(&pool->free_list));
        atomic_store(&pool->free_list, node);
    }
    
    pool->available += grow_count;
    pool->capacity = new_size;
    
    pthread_mutex_unlock(&pool->mutex);
    
    /* Recursively allocate (will use fast path now) */
    return fiber_pool_alloc(pool);
}

/* Lock-free free using atomic CAS */
void fiber_pool_free(fiber_pool_t* pool, fiber_t* fiber) {
    if (!pool || !fiber) {
        return;
    }
    
    /* Reset fiber state for reuse (keep pre-allocated stack) */
    fiber->state = FIBER_NEW;
    fiber->func = NULL;
    fiber->arg = NULL;
    fiber->result = NULL;
    fiber->parent = NULL;
    fiber->next_ready = NULL;
    fiber->prev_ready = NULL;
    fiber->affinity = 0;
    fiber->waiting_on = NULL;
    
    /* Lock-free push to free list */
    free_node_t* node = (free_node_t*)((char*)fiber - offsetof(free_node_t, fiber));
    
    free_node_t* head = atomic_load(&pool->free_list);
    do {
        atomic_store(&node->next, head);
    } while (!atomic_compare_exchange_weak(&pool->free_list, &head, node));
    
    atomic_fetch_add(&pool->available, 1);
    atomic_fetch_sub(&pool->allocated, 1);
}

size_t fiber_pool_available(fiber_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return atomic_load(&pool->available);
}

size_t fiber_pool_allocated(fiber_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return atomic_load(&pool->allocated);
}

size_t fiber_pool_capacity(fiber_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return pool->capacity;
}
