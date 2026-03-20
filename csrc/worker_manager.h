/**
 * worker_manager.h - Intelligent worker management for gsyncio
 * 
 * Provides dynamic worker scaling based on:
 * - Queue depth (workload)
 * - CPU utilization
 * - I/O wait time
 * - System load
 */

#ifndef WORKER_MANAGER_H
#define WORKER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================ */
/* Configuration                                */
/* ============================================ */

#define WORKER_MANAGER_MIN_WORKERS 2
#define WORKER_MANAGER_MAX_WORKERS_FACTOR 2  /* max = CPU_COUNT * 2 */
#define WORKER_MANAGER_SCALE_UP_THRESHOLD 100    /* Queue depth to scale up */
#define WORKER_MANAGER_SCALE_DOWN_THRESHOLD 10   /* Queue depth to scale down */
#define WORKER_MANAGER_IDLE_TIMEOUT_MS 5000      /* Scale down after 5s idle */
#define WORKER_MANAGER_CHECK_INTERVAL_MS 1000    /* Check every 1s */

/* ============================================ */
/* Worker Manager State                         */
/* ============================================ */

typedef enum {
    WORKER_STATE_IDLE = 0,
    WORKER_STATE_BUSY = 1,
    WORKER_STATE_SLEEPING = 2,
    WORKER_STATE_STOPPED = 3
} worker_state_t;

typedef struct {
    /* Current state */
    size_t current_workers;
    size_t min_workers;
    size_t max_workers;
    size_t cpu_count;
    
    /* Metrics */
    uint64_t total_tasks_spawned;
    uint64_t total_tasks_completed;
    uint64_t total_work_steals;
    uint64_t total_idle_time_ms;
    uint64_t total_busy_time_ms;
    
    /* Queue metrics */
    size_t avg_queue_depth;
    size_t max_queue_depth;
    size_t current_queue_depth;
    
    /* Scaling state */
    uint64_t last_scale_time_ms;
    uint64_t idle_start_time_ms;
    bool is_scaling;
    
    /* Configuration */
    bool auto_scaling_enabled;
    bool energy_efficient_mode;
    size_t scale_up_threshold;
    size_t scale_down_threshold;
    uint64_t idle_timeout_ms;
} worker_manager_t;

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

/**
 * Initialize worker manager
 * @param manager Manager to initialize
 * @param cpu_count Number of CPU cores
 * @return 0 on success, -1 on failure
 */
int worker_manager_init(worker_manager_t* manager, size_t cpu_count);

/**
 * Shutdown worker manager
 * @param manager Manager to shutdown
 */
void worker_manager_shutdown(worker_manager_t* manager);

/* ============================================ */
/* Dynamic Scaling                              */
/* ============================================ */

/**
 * Check if scaling is needed and adjust workers
 * @param manager Manager
 * @param current_queue_depth Current number of pending tasks
 * @return Number of workers to add (>0) or remove (<0), 0 if no change
 */
int worker_manager_check_scale(worker_manager_t* manager, size_t current_queue_depth);

/**
 * Manually scale workers
 * @param manager Manager
 * @param target_workers Target number of workers
 * @return 0 on success, -1 on failure
 */
int worker_manager_scale_to(worker_manager_t* manager, size_t target_workers);

/**
 * Get recommended worker count based on workload
 * @param manager Manager
 * @return Recommended worker count
 */
size_t worker_manager_get_recommended_workers(worker_manager_t* manager);

/* ============================================ */
/* Metrics & Monitoring                         */
/* ============================================ */

/**
 * Record task spawn event
 * @param manager Manager
 */
void worker_manager_record_spawn(worker_manager_t* manager);

/**
 * Record task completion event
 * @param manager Manager
 */
void worker_manager_record_completion(worker_manager_t* manager);

/**
 * Record work steal event
 * @param manager Manager
 */
void worker_manager_record_steal(worker_manager_t* manager);

/**
 * Record worker idle time
 * @param manager Manager
 * @param worker_id Worker ID
 * @param idle_time_ms Idle time in milliseconds
 */
void worker_manager_record_idle(worker_manager_t* manager, size_t worker_id, uint64_t idle_time_ms);

/**
 * Record worker busy time
 * @param manager Manager
 * @param worker_id Worker ID
 * @param busy_time_ms Busy time in milliseconds
 */
void worker_manager_record_busy(worker_manager_t* manager, size_t worker_id, uint64_t busy_time_ms);

/**
 * Get utilization percentage (0-100)
 * @param manager Manager
 * @return Utilization percentage
 */
double worker_manager_get_utilization(worker_manager_t* manager);

/**
 * Get load average (tasks per worker)
 * @param manager Manager
 * @return Load average
 */
double worker_manager_get_load_average(worker_manager_t* manager);

/* ============================================ */
/* Configuration                                */
/* ============================================ */

/**
 * Enable/disable auto-scaling
 * @param manager Manager
 * @param enabled Enable or disable
 */
void worker_manager_set_auto_scaling(worker_manager_t* manager, bool enabled);

/**
 * Enable energy-efficient mode (fewer workers when idle)
 * @param manager Manager
 * @param enabled Enable or disable
 */
void worker_manager_set_energy_efficient_mode(worker_manager_t* manager, bool enabled);

/**
 * Set scaling thresholds
 * @param manager Manager
 * @param scale_up_threshold Queue depth to trigger scale up
 * @param scale_down_threshold Queue depth to trigger scale down
 */
void worker_manager_set_thresholds(worker_manager_t* manager, 
                                   size_t scale_up_threshold,
                                   size_t scale_down_threshold);

/* ============================================ */
/* Utility Functions                            */
/* ============================================ */

/**
 * Get current timestamp in milliseconds
 * @return Timestamp in ms
 */
uint64_t worker_manager_get_time_ms(void);

/**
 * Get CPU count
 * @return Number of CPU cores
 */
size_t worker_manager_get_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WORKER_MANAGER_H */
