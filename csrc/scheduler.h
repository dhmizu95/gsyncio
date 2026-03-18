/**
 * scheduler.h - M:N work-stealing scheduler interface for gsyncio
 * 
 * High-performance M:N scheduler that maps M fibers onto N worker threads
 * with work-stealing for load balancing.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "fiber.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Double-ended queue (deque) for work-stealing */
typedef struct deque {
    fiber_t** data;
    size_t capacity;
    size_t top;     /* Index for local push/pop (LIFO) */
    size_t bottom;  /* Index for steal (FIFO from other end) */
} deque_t;

/* Worker thread state */
typedef struct worker {
    int id;                 /* Worker ID */
    deque_t* deque;         /* Local work deque */
    fiber_t* current_fiber; /* Currently executing fiber */
    bool running;           /* Is worker running */
    bool stopped;           /* Should worker stop */
    pthread_t thread;       /* Thread handle */
    uint64_t tasks_executed;/* Number of tasks executed */
    uint64_t steals_attempted;
    uint64_t steals_successful;
    int last_victim;       /* Last victim for work stealing */
} worker_t;

/* Scheduler configuration */
typedef struct scheduler_config {
    size_t num_workers;     /* Number of worker threads (default: CPU cores) */
    size_t max_fibers;      /* Maximum concurrent fibers */
    size_t stack_size;      /* Default fiber stack size */
    bool work_stealing;     /* Enable work stealing */
} scheduler_config_t;

/* Scheduler statistics */
typedef struct scheduler_stats {
    uint64_t total_fibers_created;
    uint64_t total_fibers_completed;
    uint64_t total_context_switches;
    uint64_t total_work_steals;
    uint64_t current_active_fibers;
    uint64_t current_ready_fibers;
} scheduler_stats_t;

/* Main scheduler structure */
typedef struct scheduler {
    worker_t* workers;          /* Array of worker threads */
    size_t num_workers;         /* Number of workers */
    size_t next_worker;         /* Round-robin index for new tasks */
    
    fiber_t* ready_queue;       /* Global ready queue (fallback) */
    fiber_t* blocked_queue;     /* Blocked fibers (I/O, channels, etc.) */
    
    scheduler_config_t config;  /* Configuration */
    scheduler_stats_t stats;    /* Statistics */
    
    bool running;               /* Is scheduler running */
    bool initialized;           /* Is scheduler initialized */
    
    /* Synchronization */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    /* Fiber pool for reuse */
    void* fiber_pool;
} scheduler_t;

/* Global scheduler instance */
extern scheduler_t* g_scheduler;

/* Scheduler API */

/**
 * Initialize the scheduler
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int scheduler_init(scheduler_config_t* config);

/**
 * Shutdown the scheduler
 * @param wait_for_completion Wait for all fibers to complete
 */
void scheduler_shutdown(bool wait_for_completion);

/**
 * Get the global scheduler instance
 * @return Scheduler instance
 */
scheduler_t* scheduler_get(void);

/**
 * Spawn a new fiber/task
 * @param entry Entry point function
 * @param user_data User data
 * @return Fiber ID, or 0 on error
 */
uint64_t scheduler_spawn(void (*entry)(void*), void* user_data);

/**
 * Schedule a fiber for execution
 * @param f Fiber to schedule
 * @param worker_id Target worker (-1 for any)
 */
void scheduler_schedule(fiber_t* f, int worker_id);

/**
 * Block the current fiber
 * @param reason Block reason (I/O, channel, etc.)
 */
void scheduler_block(void* reason);

/**
 * Unblock a fiber
 * @param f Fiber to unblock
 */
void scheduler_unblock(fiber_t* f);

/**
 * Yield the current fiber
 */
void scheduler_yield(void);

/**
 * Wait for a fiber to complete
 * @param f Fiber to wait for
 */
void scheduler_wait(fiber_t* f);

/**
 * Wait for all fibers to complete
 */
void scheduler_wait_all(void);

/**
 * Get scheduler statistics
 * @param stats Output statistics
 */
void scheduler_get_stats(scheduler_stats_t* stats);

/**
 * Get current worker ID
 * @return Worker ID, or -1 if not in worker context
 */
int scheduler_current_worker(void);

/**
 * Get number of worker threads
 * @return Number of workers
 */
size_t scheduler_num_workers(void);

/**
 * Run the scheduler main loop (called from main thread)
 */
void scheduler_run(void);

/**
 * Stop the scheduler
 */
void scheduler_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
