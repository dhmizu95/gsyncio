/**
 * worker_manager.c - Intelligent worker management for gsyncio
 * 
 * Provides dynamic worker scaling based on workload patterns.
 */

#include "worker_manager.h"
#include "scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ============================================ */
/* Utility Functions                            */
/* ============================================ */

uint64_t worker_manager_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t worker_manager_get_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (size_t)n : 1;
}

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

int worker_manager_init(worker_manager_t* manager, size_t cpu_count) {
    if (!manager) {
        return -1;
    }

    memset(manager, 0, sizeof(worker_manager_t));

    manager->cpu_count = cpu_count > 0 ? cpu_count : worker_manager_get_cpu_count();
    manager->min_workers = WORKER_MANAGER_MIN_WORKERS;
    manager->max_workers = manager->cpu_count * WORKER_MANAGER_MAX_WORKERS_FACTOR;
    manager->current_workers = manager->min_workers;  /* Start with minimum */
    
    /* Metrics */
    manager->total_tasks_spawned = 0;
    manager->total_tasks_completed = 0;
    manager->total_work_steals = 0;
    manager->total_idle_time_ms = 0;
    manager->total_busy_time_ms = 0;
    
    /* Queue metrics */
    manager->avg_queue_depth = 0;
    manager->max_queue_depth = 0;
    manager->current_queue_depth = 0;
    
    /* Scaling state */
    manager->last_scale_time_ms = worker_manager_get_time_ms();
    manager->idle_start_time_ms = 0;
    manager->is_scaling = false;
    
    /* Configuration */
    manager->auto_scaling_enabled = true;
    manager->energy_efficient_mode = false;
    manager->scale_up_threshold = WORKER_MANAGER_SCALE_UP_THRESHOLD;
    manager->scale_down_threshold = WORKER_MANAGER_SCALE_DOWN_THRESHOLD;
    manager->idle_timeout_ms = WORKER_MANAGER_IDLE_TIMEOUT_MS;

    return 0;
}

void worker_manager_shutdown(worker_manager_t* manager) {
    if (!manager) {
        return;
    }
    
    /* Scale down to minimum workers */
    manager->auto_scaling_enabled = false;
    manager->current_workers = manager->min_workers;
    
    memset(manager, 0, sizeof(worker_manager_t));
}

/* ============================================ */
/* Dynamic Scaling                              */
/* ============================================ */

int worker_manager_check_scale(worker_manager_t* manager, size_t current_queue_depth) {
    if (!manager || !manager->auto_scaling_enabled || manager->is_scaling) {
        return 0;
    }

    uint64_t now = worker_manager_get_time_ms();
    
    /* Don't scale too frequently */
    if (now - manager->last_scale_time_ms < WORKER_MANAGER_CHECK_INTERVAL_MS) {
        return 0;
    }

    manager->current_queue_depth = current_queue_depth;
    
    /* Update max queue depth */
    if (current_queue_depth > manager->max_queue_depth) {
        manager->max_queue_depth = current_queue_depth;
    }
    
    /* Update average queue depth (exponential moving average) */
    manager->avg_queue_depth = (manager->avg_queue_depth * 7 + current_queue_depth) / 8;

    int scale_change = 0;

    /* Scale UP if queue is deep */
    if (current_queue_depth > manager->scale_up_threshold) {
        if (manager->current_workers < manager->max_workers) {
            /* Add workers based on queue depth */
            size_t target = manager->current_workers + (current_queue_depth / manager->scale_up_threshold);
            if (target > manager->max_workers) {
                target = manager->max_workers;
            }
            scale_change = (int)(target - manager->current_workers);
            
            if (scale_change > 0) {
                manager->current_workers = target;
                manager->last_scale_time_ms = now;
                manager->idle_start_time_ms = 0;  /* Reset idle timer */
                
                fprintf(stderr, "[WorkerManager] Scaling UP: %zu → %zu workers (queue: %zu)\n",
                        manager->current_workers - scale_change,
                        manager->current_workers,
                        current_queue_depth);
            }
        }
    }
    /* Scale DOWN if queue is shallow and we've been idle */
    else if (current_queue_depth < manager->scale_down_threshold) {
        if (manager->idle_start_time_ms == 0) {
            manager->idle_start_time_ms = now;
        } else if (now - manager->idle_start_time_ms > manager->idle_timeout_ms) {
            if (manager->current_workers > manager->min_workers) {
                /* Remove workers gradually */
                size_t target = manager->current_workers - 1;
                if (target < manager->min_workers) {
                    target = manager->min_workers;
                }
                scale_change = (int)(target - manager->current_workers);
                
                if (scale_change < 0) {
                    manager->current_workers = target;
                    manager->last_scale_time_ms = now;
                    manager->idle_start_time_ms = now;  /* Keep idle timer running */
                    
                    fprintf(stderr, "[WorkerManager] Scaling DOWN: %zu → %zu workers (idle)\n",
                            manager->current_workers - scale_change,
                            manager->current_workers);
                }
            }
        }
    } else {
        /* Queue is moderate - reset idle timer */
        manager->idle_start_time_ms = 0;
    }

    return scale_change;
}

int worker_manager_scale_to(worker_manager_t* manager, size_t target_workers) {
    if (!manager) {
        return -1;
    }

    if (target_workers < manager->min_workers) {
        target_workers = manager->min_workers;
    }
    if (target_workers > manager->max_workers) {
        target_workers = manager->max_workers;
    }

    if (target_workers == manager->current_workers) {
        return 0;
    }

    int change = (int)(target_workers - manager->current_workers);
    manager->current_workers = target_workers;
    manager->last_scale_time_ms = worker_manager_get_time_ms();
    
    fprintf(stderr, "[WorkerManager] Manual scale: %zu workers\n", target_workers);
    
    return change;
}

size_t worker_manager_get_recommended_workers(worker_manager_t* manager) {
    if (!manager) {
        return WORKER_MANAGER_MIN_WORKERS;
    }

    /* Base recommendation on queue depth and utilization */
    double load = worker_manager_get_load_average(manager);
    
    if (load > 10.0) {
        /* High load - use more workers */
        return manager->max_workers;
    } else if (load > 1.0) {
        /* Moderate load - scale proportionally */
        size_t recommended = (size_t)(manager->cpu_count * load);
        if (recommended < manager->min_workers) {
            recommended = manager->min_workers;
        }
        if (recommended > manager->max_workers) {
            recommended = manager->max_workers;
        }
        return recommended;
    } else {
        /* Low load - use minimum */
        return manager->min_workers;
    }
}

/* ============================================ */
/* Metrics & Monitoring                         */
/* ============================================ */

void worker_manager_record_spawn(worker_manager_t* manager) {
    if (manager) {
        manager->total_tasks_spawned++;
    }
}

void worker_manager_record_completion(worker_manager_t* manager) {
    if (manager) {
        manager->total_tasks_completed++;
    }
}

void worker_manager_record_steal(worker_manager_t* manager) {
    if (manager) {
        manager->total_work_steals++;
    }
}

void worker_manager_record_idle(worker_manager_t* manager, size_t worker_id, uint64_t idle_time_ms) {
    (void)worker_id;
    if (manager) {
        manager->total_idle_time_ms += idle_time_ms;
    }
}

void worker_manager_record_busy(worker_manager_t* manager, size_t worker_id, uint64_t busy_time_ms) {
    (void)worker_id;
    if (manager) {
        manager->total_busy_time_ms += busy_time_ms;
    }
}

double worker_manager_get_utilization(worker_manager_t* manager) {
    if (!manager) {
        return 0.0;
    }

    uint64_t total_time = manager->total_idle_time_ms + manager->total_busy_time_ms;
    if (total_time == 0) {
        return 0.0;
    }

    return (double)manager->total_busy_time_ms * 100.0 / (double)total_time;
}

double worker_manager_get_load_average(worker_manager_t* manager) {
    if (!manager || manager->current_workers == 0) {
        return 0.0;
    }

    /* Load = tasks per worker */
    return (double)manager->current_queue_depth / (double)manager->current_workers;
}

/* ============================================ */
/* Configuration                                */
/* ============================================ */

void worker_manager_set_auto_scaling(worker_manager_t* manager, bool enabled) {
    if (manager) {
        manager->auto_scaling_enabled = enabled;
    }
}

void worker_manager_set_energy_efficient_mode(worker_manager_t* manager, bool enabled) {
    if (manager) {
        manager->energy_efficient_mode = enabled;
        if (enabled) {
            /* In energy-efficient mode, scale down faster */
            manager->min_workers = 1;
            manager->idle_timeout_ms = WORKER_MANAGER_IDLE_TIMEOUT_MS / 2;
        } else {
            manager->min_workers = WORKER_MANAGER_MIN_WORKERS;
            manager->idle_timeout_ms = WORKER_MANAGER_IDLE_TIMEOUT_MS;
        }
    }
}

void worker_manager_set_thresholds(worker_manager_t* manager, 
                                   size_t scale_up_threshold,
                                   size_t scale_down_threshold) {
    if (manager) {
        manager->scale_up_threshold = scale_up_threshold;
        manager->scale_down_threshold = scale_down_threshold;
    }
}
