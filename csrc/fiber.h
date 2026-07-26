/**
 * fiber.h - Fiber runtime for gsyncio
 * 
 * Based on Viper fiber runtime with modifications for gsyncio.
 * Stackful coroutines for supporting millions of concurrent tasks.
 */

#ifndef FIBER_H
#define FIBER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================ */
/* Configuration                                */
/* ============================================ */

#define FIBER_INITIAL_STACK_SIZE 2048   /* 2KB initial stack */
#define FIBER_MAX_STACK_SIZE 65536      /* 64KB max stack */
#define FIBER_STACK_GROW_STEP 4096      /* Grow by 4KB */
#define FIBER_DEFAULT_STACK_SIZE 8192   /* 8KB default - safer for Python 3.12 */
#define FIBER_USE_GUARD_PAGES 0         /* Disable guard pages to reach 1M+ system limit */

/* Whether fibers get their own mmap'd stack.
 *
 * 0, because nothing in this codebase runs on one. A fiber body is
 * invoked as a plain call - worker_thread() does `f->func(f->arg)` on
 * the worker's own OS stack - and fiber_yield() reschedules without any
 * stack switch, so stack_base/stack_ptr were allocated, paged in, and
 * freed without ever being touched.
 *
 * The cost was not theoretical: with STACK_MODE_HYBRID (the default the
 * Python binding passes) every fiber allocation mmap'd 8KB and every
 * free munmap'd it, so a million tasks meant a million mmap/munmap
 * pairs. Both take the process-wide mmap_lock and munmap additionally
 * triggers TLB shootdown IPIs to every core, which serialises all the
 * workers in the kernel. Measured: 1M create_task() went from 14.6
 * us/task (pool warm, stacks kept mapped) to 97.8 us/task once the pool
 * had to keep minting and unmapping them.
 *
 * Set back to 1 if real stack-switching fibers are ever implemented -
 * that is the only thing these fields are for. */
#define FIBER_ALLOCATE_STACKS 0
#define FIBER_POOL_LAZY_ALLOC 1         /* Lazy stack allocation for memory efficiency */

/* ============================================ */
/* Fiber States                                */
/* ============================================ */

typedef enum {
    FIBER_NEW = 0,         /* Created, not yet started */
    FIBER_READY = 1,       /* Ready to run */
    FIBER_RUNNING = 2,     /* Currently executing */
    FIBER_WAITING = 3,     /* Waiting on I/O or channel */
    FIBER_COMPLETED = 4,   /* Finished execution */
    FIBER_CANCELLED = 5    /* Cancelled */
} fiber_state_t;

/* ============================================ */
/* Stack Management Modes                       */
/* ============================================ */

typedef enum {
    STACK_MODE_NATIVE = 0,   /* Default: Keep stacks mapped for reuse (Fastest) */
    STACK_MODE_HYBRID = 1    /* Hybrid: munmap stacks when returning to pool (Saves maps) */
} fiber_stack_mode_t;

/* ============================================ */
/* Fiber Control Block                          */
/* ============================================ */

typedef struct fiber fiber_t;

struct fiber {
    /* Fiber ID */
    uint64_t id;

    /* State */
    fiber_state_t state;

    /* Stack */
    void* stack_base;          /* Bottom of stack (high address) */
    void* stack_ptr;            /* Current stack pointer */
    size_t stack_size;         /* Current stack size */
    size_t stack_capacity;      /* Allocated capacity */
    size_t mmap_size;          /* Total size passed to mmap */

    /* Function to execute */
    void (*func)(void*);
    void* arg;

    /* Return value */
    void* result;

    /* Parent fiber (who spawned this one) */
    fiber_t* parent;

    /* Scheduler link */
    fiber_t* next_ready;
    fiber_t* prev_ready;

    /* Thread affinity (0 = any) */
    int32_t affinity;

    /* Does this fiber's body need the CPython GIL to run?
     *
     * Set at spawn time: 1 for anything that calls into Python (every
     * gs.task()/gs.spawn()/coroutine fiber), 0 for pure-C bodies
     * (c_tasks). The scheduler keeps the two classes on separate run
     * queues and separate worker threads, because they want opposite
     * things: nogil fibers scale with worker count, GIL-bound fibers are
     * serialized by the interpreter anyway and get *slower* with every
     * extra worker (each task boundary becomes a GIL handoff between OS
     * threads - a futex sleep/wake pair). Measured on 12 cores: 0.86
     * us/task nogil flat across 1-12 workers, vs 13 us/task on one
     * worker and 87 us/task on twelve for a Python body. */
    int32_t gil_bound;

    /* Fiber pool (for pooled allocation) */
    void* pool;

    /* Debug info */
    const char* name;

    /* Context switching */
    jmp_buf context;         /* Saved context for switch */

    /* Async/await support */
    void* waiting_on;           /* What fiber is waiting on (Future, Channel, etc.) */
};

/* ============================================ */
/* Fiber API                                   */
/* ============================================ */

/**
 * Initialize fiber subsystem
 * @return 0 on success, -1 on failure
 */
int fiber_init(void);

/**
 * Cleanup fiber subsystem
 */
void fiber_cleanup(void);

/**
 * Create a new fiber
 * @param func Function to execute
 * @param arg Argument to pass to function
 * @param stack_size Initial stack size (0 = default)
 * @return New fiber, or NULL on failure
 */
fiber_t* fiber_create(void (*func)(void*), void* arg, size_t stack_size);

/**
 * Free a fiber
 * @param fiber Fiber to free
 */
void fiber_free(fiber_t* fiber);

/**
 * Yield execution to scheduler
 */
void fiber_yield(void);

/**
 * Resume a fiber
 * @param fiber Fiber to resume
 */
void fiber_resume(fiber_t* fiber);

/**
 * Get current running fiber
 * @return Current fiber, or NULL if on main thread
 */
fiber_t* fiber_current(void);

/**
 * Set current running fiber for this OS thread (TLS)
 * Called by the scheduler's worker loop around fiber execution, since
 * fibers here run as plain calls on the worker's own stack rather than
 * through a stack-switching fiber_switch().
 * @param f Fiber now running on this thread, or NULL when none is
 */
void fiber_set_current(fiber_t* f);

/**
 * Add fiber to tracking table
 * @param f Fiber to add
 * @return 0 on success, -1 on failure
 */
int fiber_table_add(fiber_t* f);

/**
 * Switch to another fiber
 * @param from Fiber to switch from
 * @param to Fiber to switch to
 */
void fiber_switch(fiber_t* from, fiber_t* to);

/**
 * Get fiber ID
 * @param fiber Fiber
 * @return Fiber ID
 */
uint64_t fiber_id(fiber_t* fiber);

/**
 * Get fiber state
 * @param fiber Fiber
 * @return Current state
 */
fiber_state_t fiber_state(fiber_t* fiber);

/* ============================================ */
/* Fiber Parking (for async I/O)               */
/* ============================================ */

/**
 * Park current fiber (yield and wait to be resumed)
 */
void fiber_park(void);

/**
 * Unpark a fiber
 * @param fiber Fiber to resume
 */
void fiber_unpark(fiber_t* fiber);

/**
 * Check if fiber is parked
 * @param fiber Fiber to check
 * @return true if parked
 */
bool fiber_is_parked(fiber_t* fiber);

#ifdef __cplusplus
}
#endif

#endif /* FIBER_H */
