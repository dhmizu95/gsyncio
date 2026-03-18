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

#define FIBER_INITIAL_STACK_SIZE 1024   /* 1KB initial stack - minimal for most tasks */
#define FIBER_MAX_STACK_SIZE 32768     /* 32KB max stack */
#define FIBER_STACK_GROW_STEP 2048      /* Grow by 2KB */
#define FIBER_DEFAULT_STACK_SIZE 2048   /* 2KB default - like Go goroutines */
#define FIBER_USE_GUARD_PAGES 1         /* Enable guard pages for memory safety */

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
 * Start executing a fiber
 * @param fiber Fiber to start
 * @return 0 on success, -1 on failure
 */
int fiber_start(fiber_t* fiber);

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

/**
 * Get fiber by ID
 * @param id Fiber ID
 * @return Fiber, or NULL if not found
 */
fiber_t* fiber_get_by_id(uint64_t id);

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
