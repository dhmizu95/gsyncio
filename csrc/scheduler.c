/**
 * scheduler.c - M:N work-stealing scheduler implementation for gsyncio
 *
 * High-performance M:N scheduler that maps M fibers onto N worker threads
 * with work-stealing for load balancing. Includes io_uring integration.
 */

#define _GNU_SOURCE  /* For CPU_ZERO, CPU_SET, pthread_setaffinity_np */

#include "scheduler.h"
#include "fiber_pool.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <time.h>
#include <sched.h>
#include <signal.h>
#include <execinfo.h>
#include <stdlib.h>

#ifdef __linux__
#include "io_uring.h"
#endif

/* ============================================ */
/* Debug Logging Infrastructure                 */
/* ============================================ */

/* Enable debug logging via GSYNCIO_DEBUG environment variable */
static int g_debug_enabled = -1;

static void init_debug_flag(void) {
    if (g_debug_enabled == -1) {
        const char* env = getenv("GSYNCIO_DEBUG");
        g_debug_enabled = (env && strcmp(env, "1") == 0) ? 1 : 0;
    }
}

#define DEBUG_LOG(fmt, ...) do { \
    if (g_debug_enabled == -1) init_debug_flag(); \
    if (g_debug_enabled) { \
        fprintf(stderr, "[DEBUG %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        fflush(stderr); \
    } \
} while(0)

#define DEBUG_LOG_FIBER(msg, fiber) do { \
    if (g_debug_enabled == -1) init_debug_flag(); \
    if (g_debug_enabled) { \
        fprintf(stderr, "[DEBUG %s:%d] Fiber %lu: " msg " (state=%d, stack=%p)\n", \
            __FILE__, __LINE__, \
            (fiber) ? (unsigned long)(fiber)->id : 0, \
            (fiber) ? (int)(fiber)->state : -1, \
            (fiber) ? (fiber)->stack_base : NULL); \
        fflush(stderr); \
    } \
} while(0)

/* ============================================ */
/* Segfault Handler with Stack Trace           */
/* ============================================ */

static void sigsegv_handler(int sig, siginfo_t* info, void* context) {
    (void)context;
    
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "FATAL: Segmentation fault (signal %d)\n", sig);
    fprintf(stderr, "Fault address: %p\n", info->si_addr);
    fprintf(stderr, "========================================\n");
    
    /* Print stack trace */
    void* buffer[64];
    int n = backtrace(buffer, 64);
    fprintf(stderr, "Stack trace (%d frames):\n", n);
    backtrace_symbols_fd(buffer, n, 2);
    
    /* Print scheduler stats if available */
    if (g_scheduler) {
        scheduler_t* s = (scheduler_t*)g_scheduler;
        fprintf(stderr, "\nScheduler stats:\n");
        fprintf(stderr, "  Active tasks: %lu\n", (unsigned long)s->stats.atomic_task_count);
        fprintf(stderr, "  Total fibers completed: %lu\n", (unsigned long)s->stats.total_fibers_completed);
    }
    
    fprintf(stderr, "========================================\n");
    fflush(stderr);
    
    _exit(1);
}

static void install_crash_handler(void) {
    static int installed = 0;
    if (installed) return;
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    
    installed = 1;
    DEBUG_LOG("Crash handler installed");
}

/* Fast sleep threshold: use spin-wait for sleeps under this value (1ms) */
#define FAST_SLEEP_THRESHOLD_NS 1000000ULL

/* Spin count before yielding in fast sleep */
#define FAST_SLEEP_SPIN_COUNT 1000

scheduler_t* g_scheduler = NULL;

/* Pre-allocated timer pool for lock-free timer allocation */
typedef struct timer_pool {
    timer_node_t* nodes;
    _Atomic timer_node_t* free_list;
    size_t capacity;
} timer_pool_t;

static timer_pool_t g_timer_pool;

static void timer_pool_init(size_t capacity) {
    g_timer_pool.capacity = capacity;
    g_timer_pool.nodes = (timer_node_t*)calloc(capacity, sizeof(timer_node_t));
    if (!g_timer_pool.nodes) {
        return;
    }
    atomic_store(&g_timer_pool.free_list, NULL);

    /* Pre-populate free list */
    for (size_t i = 0; i < capacity; i++) {
        g_timer_pool.nodes[i].next = atomic_load(&g_timer_pool.free_list);
        atomic_store(&g_timer_pool.free_list, &g_timer_pool.nodes[i]);
    }
}

static timer_node_t* timer_pool_alloc(void) {
    timer_node_t* node = atomic_load(&g_timer_pool.free_list);
    while (node != NULL) {
        timer_node_t* next = node->next;  /* Regular load - node->next is not atomic */
        if (atomic_compare_exchange_weak(&g_timer_pool.free_list, &node, next)) {
            return node;
        }
        node = atomic_load(&g_timer_pool.free_list);
    }
    /* Pool exhausted - fall back to malloc */
    return (timer_node_t*)malloc(sizeof(timer_node_t));
}

static void timer_pool_free(timer_node_t* node) {
    if (!node) return;

    /* Check if node is from pool */
    if (node >= g_timer_pool.nodes && node < g_timer_pool.nodes + g_timer_pool.capacity) {
        timer_node_t* old_head;
        do {
            old_head = atomic_load(&g_timer_pool.free_list);
            node->next = old_head;  /* Regular store - node->next is not atomic */
        } while (!atomic_compare_exchange_weak(&g_timer_pool.free_list, &old_head, node));
    } else {
        free(node);
    }
}

/* ============================================ */
/* Lock-Free Atomic Operations Implementation  */
/* ============================================ */

uint64_t scheduler_atomic_inc_task_count(void) {
    if (!g_scheduler) return 0;
    return __atomic_add_fetch(&g_scheduler->stats.atomic_task_count, 1, __ATOMIC_SEQ_CST);
}

uint64_t scheduler_atomic_dec_task_count(void) {
    if (!g_scheduler) return 0;
    return __atomic_sub_fetch(&g_scheduler->stats.atomic_task_count, 1, __ATOMIC_SEQ_CST);
}

uint64_t scheduler_atomic_get_task_count(void) {
    if (!g_scheduler) return 0;
    return __atomic_load_n(&g_scheduler->stats.atomic_task_count, __ATOMIC_SEQ_CST);
}

uint64_t scheduler_atomic_inc_fibers_spawned(void) {
    if (!g_scheduler) return 0;
    return __atomic_add_fetch(&g_scheduler->stats.atomic_fibers_spawned, 1, __ATOMIC_SEQ_CST);
}

uint64_t scheduler_atomic_inc_fibers_completed(void) {
    if (!g_scheduler) return 0;
    return __atomic_add_fetch(&g_scheduler->stats.atomic_fibers_completed, 1, __ATOMIC_SEQ_CST);
}

int scheduler_atomic_all_tasks_complete(void) {
    if (!g_scheduler) return 1;
    return __atomic_load_n(&g_scheduler->stats.atomic_task_count, __ATOMIC_SEQ_CST) == 0 ? 1 : 0;
}

/* ============================================ */
/* Sharded Counter Implementation (Low Contention) */
/* ============================================ */

/* Forward declaration for get_time_ns */
static uint64_t get_time_ns(void);

/* Recalculate total from shards */
static void sharded_counter_recalc(sharded_counter_t* sc) {
    uint64_t total = 0;
    for (int i = 0; i < NUM_SHARDS; i++) {
        total += atomic_load(&sc->counts[i]);
    }
    atomic_store(&sc->total, total);
    sc->last_update = get_time_ns();
}

/* Get total with lazy recalculation (call periodically, not on every access) */
uint64_t sharded_counter_get_total(sharded_counter_t* sc) {
    uint64_t cached = atomic_load(&sc->total);
    
    /* If we have a recent cached value, use it */
    if (cached > 0) {
        uint64_t now = get_time_ns();
        /* Cache is valid for 1 second */
        if (now - sc->last_update < 1000000000ULL) {
            return cached;
        }
    }
    
    /* Recalculate */
    sharded_counter_recalc(sc);
    return atomic_load(&sc->total);
}

/* Sharded task count operations - use worker_id for low contention */
uint64_t scheduler_sharded_inc_task_count(uint32_t worker_id) {
    if (!g_scheduler) return 0;
    return sharded_counter_inc(&g_scheduler->sharded_task_count, worker_id);
}

uint64_t scheduler_sharded_dec_task_count(uint32_t worker_id) {
    if (!g_scheduler) return 0;
    return sharded_counter_dec(&g_scheduler->sharded_task_count, worker_id);
}

uint64_t scheduler_sharded_get_task_count(void) {
    if (!g_scheduler) return 0;
    return sharded_counter_get_total(&g_scheduler->sharded_task_count);
}

/* Thread-local worker ID storage */
static __thread int t_current_worker_id = -1;

void scheduler_set_current_worker_id(int worker_id) {
    t_current_worker_id = worker_id;
}

uint32_t scheduler_get_current_worker_id(void) {
    if (t_current_worker_id < 0) {
        /* Not in a worker thread, use hash of thread ID as fallback */
        return (uint32_t)(pthread_self() % NUM_SHARDS);
    }
    return (uint32_t)t_current_worker_id;
}

static void* worker_thread(void* arg);
static fiber_t* steal_from_worker(worker_t* thief, int victim_id);
static void push_local(worker_t* w, fiber_t* f);
static fiber_t* pop_local(worker_t* w);
static void process_io_completions(scheduler_t *sched);
static void process_timers(scheduler_t *sched);
static int select_victim_adaptive(worker_t* thief);

/* Forward declaration of Python callback wrapper (from _gsyncio_core.pyx) */

static size_t get_num_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (size_t)n : 1;
}

/* ============================================ */
/* High-Resolution Timer                        */
/* ============================================ */

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int deque_init(deque_t* dq, size_t capacity) {
    dq->data = (fiber_t**)calloc(capacity, sizeof(fiber_t*));
    if (!dq->data) {
        return -1;
    }
    dq->capacity = capacity;
    dq->top = 0;
    dq->bottom = 0;
    return 0;
}

static void push_top(deque_t* dq, fiber_t* f) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    /* Check if we need to resize */
    if (b - t >= dq->capacity) {
        size_t new_capacity = dq->capacity * 2;
        fiber_t** new_data = (fiber_t**)realloc(dq->data, new_capacity * sizeof(fiber_t*));
        if (!new_data) {
            return;
        }
        dq->data = new_data;
        dq->capacity = new_capacity;
    }

    /* Store fiber FIRST (before updating bottom) */
    dq->data[b] = f;
    
    /* Full memory barrier to ensure store is visible before bottom update */
    atomic_thread_fence(memory_order_seq_cst);
    
    /* Now increment bottom */
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
}

/* Simple deque operations (single-owner) */

static fiber_t* pop_top(deque_t* dq) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    size_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);

    if (t >= b) {
        return NULL;  /* Empty */
    }

    fiber_t* f = dq->data[t];
    atomic_store_explicit(&dq->top, t + 1, memory_order_relaxed);
    return f;
}

static fiber_t* steal_bottom(deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);

    if (t >= b) {
        return NULL;  /* Empty */
    }

    fiber_t* f = dq->data[t];
    atomic_store_explicit(&dq->top, t + 1, memory_order_release);
    return f;
}

static bool deque_empty(deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    return t >= b;
}

static void process_io_completions(scheduler_t *sched) {
#ifdef __linux__
    if (!sched->io_uring_enabled) {
        return;
    }

    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&sched->io_uring_ring, &cqe) == 1) {
        uint64_t user_data = cqe->user_data;
        (void)cqe->res;  /* result handled via fiber wakeup */

        io_uring_cqe_seen(&sched->io_uring_ring, cqe);
        
        io_uring_submission_t **prev = &sched->pending_submissions;
        io_uring_submission_t *sub = sched->pending_submissions;
        while (sub) {
            if (sub->user_data == user_data) {
                *prev = sub->next;
                
                if (sub->fiber && sub->fiber->state == FIBER_WAITING) {
                    sub->fiber->state = FIBER_READY;
                    sub->fiber->waiting_on = NULL;
                    scheduler_schedule(sub->fiber, -1);
                }
                
                free(sub);
                break;
            }
            prev = &sub->next;
            sub = sub->next;
        }
        
        sched->stats.total_io_completed++;
    }
#endif
}

/* ============================================ */
/* Timer Processing                             */
/* ============================================ */

static void process_timers(scheduler_t *sched) {
    if (!sched || !sched->timers) {
        return;
    }

    uint64_t now = get_time_ns();

    pthread_mutex_lock(&sched->timers_mutex);

    timer_node_t **prev = &sched->timers;
    timer_node_t *node = sched->timers;

    while (node) {
        timer_node_t *next = node->next;  /* Save next before potentially freeing */
        
        if (node->active && node->deadline_ns <= now) {
            /* Timer expired - wake the fiber */
            *prev = next;  /* Remove from list */

            node->fiber->state = FIBER_READY;
            node->fiber->waiting_on = NULL;

            /* Schedule fiber to run */
            scheduler_schedule(node->fiber, -1);

            timer_pool_free(node);
            sched->stats.total_context_switches++;
            /* prev stays the same since we removed current node */
        } else {
            prev = &node->next;  /* Move prev forward only if keeping node */
        }
        node = next;
    }

    pthread_mutex_unlock(&sched->timers_mutex);
}

/* ============================================ */
/* Native Sleep Implementation                  */
/* ============================================ */

/**
 * Native sleep - puts current fiber to sleep for specified nanoseconds
 * 
 * Optimizations:
 * - Fast spin-wait for short sleeps (<1ms) - avoids context switch overhead
 * - Pre-allocated timer pool - avoids malloc in hot path
 * - Proper fiber yielding - fiber actually sleeps until timer expires
 */
void scheduler_sleep_ns(uint64_t ns) {
    fiber_t* current = fiber_current();
    if (!current) {
        return;  /* Not in fiber context - return immediately */
    }
    
    /* Fast path: spin-wait for short sleeps */
    if (ns < FAST_SLEEP_THRESHOLD_NS) {
        uint64_t deadline = get_time_ns() + ns;
        for (int i = 0; i < FAST_SLEEP_SPIN_COUNT; i++) {
            if (get_time_ns() >= deadline) {
                return;  /* Sleep completed */
            }
            __asm__ __volatile__("" ::: "memory");
        }
        /* Fall through to timer-based sleep if spin didn't complete */
    }
    
    /* Slow path: use timer and yield */
    uint64_t deadline = get_time_ns() + ns;

    timer_node_t* node = timer_pool_alloc();
    if (!node) {
        /* Fallback - just yield if we can't allocate timer */
        fiber_yield();
        return;
    }

    node->deadline_ns = deadline;
    node->fiber = current;
    node->active = true;
    atomic_store(&node->next, NULL);

    /* Mark fiber as waiting and add timer */
    current->state = FIBER_WAITING;
    current->waiting_on = node;

    /* Add to timer list (lock-free push) */
    pthread_mutex_lock(&g_scheduler->timers_mutex);
    atomic_store(&node->next, g_scheduler->timers);
    g_scheduler->timers = node;
    pthread_mutex_unlock(&g_scheduler->timers_mutex);

    /* CRITICAL: Actually yield the fiber to the scheduler */
    fiber_yield();
}

static void* worker_thread(void* arg) {
    worker_t* w = (worker_t*)arg;
    scheduler_t* sched = g_scheduler;

    /* Set thread-local worker ID for sharded counter operations */
    scheduler_set_current_worker_id(w->id);

    // Signal that worker has started
    atomic_store(&w->started, true);

    // Warmup - do a quick spin to avoid cold-start latency
    for (int i = 0; i < 1000; i++) {
        __asm__ __volatile__("" ::: "memory");
    }

    while (w->running) {
        fiber_t* f = NULL;

        // Try local queue first (fast path - no lock needed)
        f = pop_local(w);

        // Try adaptive work-stealing if no local work
        if (!f && sched->config.work_stealing) {
            // First try adaptive selection (steal from busiest)
            int victim = select_victim_adaptive(w);
            if (victim >= 0) {
                f = steal_from_worker(w, victim);
            }

            // Fall back to round-robin if adaptive failed
            if (!f) {
                for (size_t i = 0; i < sched->num_workers; i++) {
                    int victim_id = (w->id + i + 1) % sched->num_workers;
                    f = steal_from_worker(w, victim_id);
                    if (f) break;
                }
            }
        }

        // Try global queue as last resort
        if (!f) {
            pthread_mutex_lock(&sched->mutex);
            f = sched->ready_queue;
            if (f) {
                sched->ready_queue = f->next_ready;
            }
            pthread_mutex_unlock(&sched->mutex);
        }

        if (f) {
            /* Check fiber state hasn't changed (prevent double execution) */
            fiber_state_t expected_state = f->state;
            if (expected_state != FIBER_NEW && expected_state != FIBER_READY) {
                /* Fiber already being processed by another worker */
                w->current_fiber = NULL;
                continue;
            }
            
            /* Atomically try to claim the fiber */
            if (!__atomic_compare_exchange_n(&f->state, &expected_state, FIBER_RUNNING,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                /* Another worker claimed it first */
                w->current_fiber = NULL;
                continue;
            }
            
            w->current_fiber = f;
            w->tasks_executed++;

            DEBUG_LOG_FIBER("Executing fiber", f);
            
            /* Check for NULL stack - potential crash source */
            if (!f->stack_base && !f->stack_ptr) {
                fprintf(stderr, "[ERROR] Worker %d: Fiber %lu has NULL stack! (state=%d, pool=%p)\n",
                        w->id, (unsigned long)f->id, (int)f->state, f->pool);
                fflush(stderr);
            }

            if (f->state == FIBER_RUNNING) {
                if (expected_state == FIBER_NEW) {
                    /* First time running this fiber */
                    DEBUG_LOG("Worker %d: Running NEW fiber %lu", w->id, (unsigned long)f->id);
                    if (setjmp(f->context) == 0) {
                        f->func(f->arg);
                        
                        /* Fiber completed - clean up */
                        f->state = FIBER_COMPLETED;

                        /* Lock-free atomic increment */
                        scheduler_atomic_inc_fibers_completed();
                        /* Decrement global atomic task count - this is what sync() waits on */
                        scheduler_atomic_dec_task_count();
                        /* Sharded counter decrement - uses thread-local worker_id for low contention */
                        scheduler_sharded_dec_task_count(scheduler_get_current_worker_id());

                        if (f->parent) {
                            scheduler_schedule(f->parent, -1);
                        }

                        DEBUG_LOG_FIBER("Freeing fiber", f);
                        
                        if (f->pool) {
                            fiber_pool_free(f->pool, f);
                        } else {
                            fiber_free(f);
                        }

                        w->current_fiber = NULL;
                        
                        DEBUG_LOG("Worker %d: Task count now %lu", w->id, (unsigned long)atomic_load(&g_scheduler->stats.atomic_task_count));
                        
                        /* Yield to allow other workers to acquire GIL */
                        sched_yield();
                        
                        continue;  /* Pick next fiber */
                    }
                    /* Fiber resumed here after yield */
                } else {
                    /* Resume existing fiber at its yield point */
                    longjmp(f->context, 1);
                }
            }

            w->current_fiber = NULL;
        } else {
            // No work - process timers and sleep
            process_timers(sched);

            int found_work = 0;

            // Brief spin before sleeping - check local and global queues
            for (int spin = 0; spin < 100 && !w->stopped && !found_work; spin++) {
                __asm__ __volatile__("" ::: "memory");

                // Check local queue first (fast path - no lock needed)
                f = pop_local(w);
                if (f) {
                    found_work = 1;
                } else {
                    // Check global queue
                    pthread_mutex_lock(&sched->mutex);
                    f = sched->ready_queue;
                    if (f) {
                        sched->ready_queue = f->next_ready;
                        found_work = 1;
                    }
                    pthread_mutex_unlock(&sched->mutex);
                }
            }

            // Execute work if found during spin
            if (found_work && f) {
                w->current_fiber = f;
                w->tasks_executed++;

                if (f->state == FIBER_NEW || f->state == FIBER_READY) {
                    if (f->state == FIBER_NEW) {
                        if (setjmp(f->context) == 0) {
                            f->state = FIBER_RUNNING;
                            f->func(f->arg);
                            f->state = FIBER_COMPLETED;
                            sched->stats.total_fibers_completed++;
                            
                            /* Decrement global atomic task count */
                            scheduler_atomic_dec_task_count();
                            /* Sharded counter decrement */
                            scheduler_sharded_dec_task_count(scheduler_get_current_worker_id());

                            if (f->parent) {
                                scheduler_schedule(f->parent, -1);
                            }

                            if (f->pool) {
                                fiber_pool_free(f->pool, f);
                            } else {
                                fiber_free(f);
                            }

                            w->current_fiber = NULL;
                            
                            /* Yield to allow other workers to acquire GIL */
                            sched_yield();
                            
                            continue;
                        }
                    } else {
                        f->state = FIBER_RUNNING;
                        longjmp(f->context, 1);
                    }
                }
                w->current_fiber = NULL;
            }

            // Sleep with timeout if no work found
            if (!found_work && !w->stopped) {
                /* Yield before sleeping to let other threads run */
                sched_yield();
                
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 1000000; // 1ms timeout

                pthread_mutex_lock(&sched->mutex);
                pthread_cond_timedwait(&sched->cond, &sched->mutex, &ts);
                pthread_mutex_unlock(&sched->mutex);
            }
        }

        if (w->stopped) {
            break;
        }
    }

    return NULL;
}

static void push_local(worker_t* w, fiber_t* f) {
    push_top(w->deque, f);
}

static fiber_t* pop_local(worker_t* w) {
    return pop_top(w->deque);
}

/* ============================================ */
/* Adaptive Work-Stealing                       */
/* ============================================ */

static inline size_t deque_size(deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    return (b > t) ? (b - t) : 0;
}

/* Select victim based on load - steal from busiest worker */
static int select_victim_adaptive(worker_t* thief) {
    scheduler_t* sched = g_scheduler;
    if (sched->num_workers <= 1) {
        return -1;
    }

    size_t max_size = 0;
    int victim = -1;

    /* Find worker with most work */
    for (size_t i = 0; i < sched->num_workers; i++) {
        if (i == (size_t)thief->id) continue;

        worker_t* w = &sched->workers[i];
        size_t size = deque_size(w->deque);

        /* Only consider stealing if worker has > 2 tasks */
        if (size > max_size && size >= 2) {
            max_size = size;
            victim = (int)i;
        }
    }

    return victim;
}

static fiber_t* steal_from_worker(worker_t* thief, int victim_id) {
    scheduler_t* sched = g_scheduler;
    if (victim_id < 0 || victim_id >= (int)sched->num_workers) {
        return NULL;
    }

    worker_t* victim = &sched->workers[victim_id];
    deque_t* dq = victim->deque;

    /* Quick check before attempting steal */
    if (deque_size(dq) == 0) {
        return NULL;
    }

    thief->steals_attempted++;

    fiber_t* f = steal_bottom(victim->deque);
    if (f) {
        thief->steals_successful++;
        sched->stats.total_work_steals++;
    }

    return f;
}

static int get_random_victim(worker_t* w) {
    scheduler_t* sched = g_scheduler;
    if (sched->num_workers <= 1) {
        return -1;
    }

    /* Use worker ID and thread address for seed - more robust initialization */
    static _Thread_local unsigned int seed = 0;
    if (seed == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed = (unsigned int)((uintptr_t)w ^ (uint64_t)ts.tv_nsec ^ (w->id + 1));
        if (seed == 0) seed = 1;  /* Ensure non-zero */
    }

    int victim = w->last_victim;
    int attempts = 0;

    do {
        victim = (victim + 1 + (rand_r(&seed) % (sched->num_workers - 1))) % sched->num_workers;
        attempts++;
    } while (victim == w->id && attempts < (int)sched->num_workers);

    w->last_victim = victim;
    return victim;
}

int scheduler_init(scheduler_config_t* config) {
    /* Install crash handler first */
    install_crash_handler();
    
    DEBUG_LOG("Scheduler initialization starting...");
    
    if (g_scheduler) {
        return -1;
    }

    scheduler_t* sched = (scheduler_t*)calloc(1, sizeof(scheduler_t));
    if (!sched) {
        return -1;
    }

    if (config) {
        sched->config = *config;
        // Auto-detect CPU cores if num_workers is 0
        if (sched->config.num_workers == 0) {
            sched->config.num_workers = get_num_cpus();
        }
    } else {
        sched->config.num_workers = get_num_cpus();
        sched->config.max_fibers = 10000000;  /* 10M fibers for high concurrency */
        sched->config.stack_size = FIBER_DEFAULT_STACK_SIZE;
        sched->config.work_stealing = true;
        sched->config.backend = SCHEDULER_BACKEND_DEFAULT;
        sched->config.io_uring_entries = 256;
    }

    sched->num_workers = sched->config.num_workers;
    sched->backend = sched->config.backend;

    sched->workers = (worker_t*)calloc(sched->num_workers, sizeof(worker_t));
    if (!sched->workers) {
        free(sched);
        return -1;
    }

    for (size_t i = 0; i < sched->num_workers; i++) {
        worker_t* w = &sched->workers[i];
        w->id = (int)i;
        atomic_store(&w->running, false);
        atomic_store(&w->started, false);
        atomic_store(&w->stopped, false);
        w->current_fiber = NULL;
        w->tasks_executed = 0;
        w->steals_attempted = 0;
        w->steals_successful = 0;

        w->deque = (deque_t*)calloc(1, sizeof(deque_t));
        if (!w->deque) {
            for (size_t j = 0; j < i; j++) {
                if (sched->workers[j].deque) {
                    free(sched->workers[j].deque->data);
                    free(sched->workers[j].deque);
                }
            }
            free(sched->workers);
            free(sched);
            return -1;
        }

        if (deque_init(w->deque, 65536) != 0) {  /* 64K initial capacity per worker */
            free(w->deque);
            for (size_t j = 0; j < i; j++) {
                if (sched->workers[j].deque) {
                    free(sched->workers[j].deque->data);
                    free(sched->workers[j].deque);
                }
            }
            free(sched->workers);
            free(sched);
            return -1;
        }
    }
    
    pthread_mutex_init(&sched->mutex, NULL);
    pthread_cond_init(&sched->cond, NULL);

    /* Initialize worker manager */
    worker_manager_init(&sched->worker_manager, sched->num_workers);

    /* Initialize sharded counters for low-contention task counting */
    sharded_counter_init(&sched->sharded_task_count);
    sharded_counter_init(&sched->sharded_completion_count);

    /* Start with 8K fibers - grows on demand to 10M */
    sched->fiber_pool = fiber_pool_create(8192);
    DEBUG_LOG("Fiber pool created: capacity=%zu", fiber_pool_capacity(sched->fiber_pool));

    /* Initialize timer pool - start with 8K timers */
    timer_pool_init(8192);

    sched->ready_queue = NULL;
    sched->blocked_queue = NULL;

    sched->fd_table_size = FD_TABLE_SIZE;
    sched->fd_table = (fd_entry_t*)calloc(sched->fd_table_size, sizeof(fd_entry_t));
    if (!sched->fd_table) {
        for (size_t i = 0; i < sched->num_workers; i++) {
            free(sched->workers[i].deque->data);
            free(sched->workers[i].deque);
        }
        free(sched->workers);
        free(sched);
        return -1;
    }

    pthread_mutex_init(&sched->pollers_mutex, NULL);
    pthread_mutex_init(&sched->timers_mutex, NULL);

#ifdef __linux__
    sched->io_uring_enabled = false;
    if (sched->config.backend == SCHEDULER_BACKEND_IOURING ||
        (sched->config.backend == SCHEDULER_BACKEND_DEFAULT)) {
        if (io_uring_init(&sched->io_uring_ring, sched->config.io_uring_entries) == 0) {
            sched->io_uring_enabled = true;
            sched->backend = SCHEDULER_BACKEND_IOURING;
            pthread_mutex_init(&sched->io_uring_mutex, NULL);
        }
    }
#endif

    fiber_init();

    // Set g_scheduler BEFORE creating worker threads
    // so worker threads can access it immediately
    g_scheduler = sched;
    sched->running = true;
    sched->initialized = true;

    // Create worker threads with CPU affinity
    // Set running=true BEFORE creating threads so they start processing immediately
    size_t num_cpus = get_num_cpus();
    for (size_t i = 0; i < sched->num_workers; i++) {
        atomic_store(&sched->workers[i].running, true);
        pthread_create(&sched->workers[i].thread, NULL, worker_thread, &sched->workers[i]);

        // Pin thread to CPU core for better cache locality
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i % num_cpus, &cpuset);
        pthread_setaffinity_np(sched->workers[i].thread, sizeof(cpuset), &cpuset);
#endif
    }

    // Wait for all workers to signal they've started (with timeout)
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (size_t i = 0; i < sched->num_workers; i++) {
        int timeout_count = 0;
        while (!atomic_load(&sched->workers[i].started) && timeout_count < 100) {
            struct timespec ts = {0, 1000000};  // 1ms
            nanosleep(&ts, NULL);
            timeout_count++;
        }
    }

    return 0;
}

void scheduler_shutdown(bool wait_for_completion) {
    scheduler_t* sched = g_scheduler;
    if (!sched || !sched->initialized) {
        return;
    }

    sched->running = false;

    if (wait_for_completion) {
        scheduler_wait_all();
    }

    pthread_mutex_lock(&sched->mutex);
    for (size_t i = 0; i < sched->num_workers; i++) {
        atomic_store(&sched->workers[i].stopped, true);
        atomic_store(&sched->workers[i].running, false);
    }
    pthread_cond_broadcast(&sched->cond);
    pthread_mutex_unlock(&sched->mutex);
    
    for (size_t i = 0; i < sched->num_workers; i++) {
        pthread_join(sched->workers[i].thread, NULL);
        if (sched->workers[i].deque) {
            free(sched->workers[i].deque->data);
            free(sched->workers[i].deque);
        }
    }
    
    pthread_mutex_destroy(&sched->mutex);
    pthread_cond_destroy(&sched->cond);
    
    if (sched->fd_table) {
        free(sched->fd_table);
    }
    
    pthread_mutex_destroy(&sched->pollers_mutex);
    pthread_mutex_destroy(&sched->timers_mutex);
    
#ifdef __linux__
    if (sched->io_uring_enabled) {
        io_uring_destroy(&sched->io_uring_ring);
        pthread_mutex_destroy(&sched->io_uring_mutex);
        
        io_uring_submission_t *sub = sched->pending_submissions;
        while (sub) {
            io_uring_submission_t *next = sub->next;
            free(sub);
            sub = next;
        }
    }
#endif
    
    /* Clean up remaining timers */
    timer_node_t* node = sched->timers;
    while (node) {
        timer_node_t* next = atomic_load(&node->next);
        timer_pool_free(node);
        node = next;
    }
    
    /* Free timer pool nodes array */
    if (g_timer_pool.nodes) {
        free(g_timer_pool.nodes);
        g_timer_pool.nodes = NULL;
    }
    
    fiber_pool_destroy(sched->fiber_pool);
    worker_manager_shutdown(&sched->worker_manager);
    fiber_cleanup();
    
    free(sched->workers);
    free(sched);
    g_scheduler = NULL;
}

scheduler_t* scheduler_get(void) {
    return g_scheduler;
}

uint64_t scheduler_spawn(void (*entry)(void*), void* user_data) {
    if (!g_scheduler || !entry) {
        return 0;
    }

    fiber_t* f = NULL;

    /* Try fiber pool first for faster allocation */
    if (g_scheduler->fiber_pool) {
        f = fiber_pool_alloc((fiber_pool_t*)g_scheduler->fiber_pool);
        if (f) {
            /* Initialize fiber fields */
            f->func = entry;
            f->arg = user_data;
            f->parent = fiber_current();

            /* Lazy stack allocation - allocate now if needed */
            if (!f->stack_base) {
                size_t stack_size = g_scheduler->config.stack_size > 0 ?
                    g_scheduler->config.stack_size : FIBER_DEFAULT_STACK_SIZE;
                f->stack_base = mmap(
                    NULL,
                    stack_size + 4096,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1,
                    0
                );
                if (f->stack_base == MAP_FAILED) {
                    fiber_pool_free((fiber_pool_t*)g_scheduler->fiber_pool, f);
                    return 0;
                }
                mprotect(f->stack_base, 4096, PROT_NONE);
                f->stack_size = stack_size;
                f->stack_capacity = stack_size;
                f->stack_ptr = (char*)f->stack_base + stack_size + 4096;
            }
        }
    }

    /* Fall back to direct allocation if pool exhausted */
    if (!f) {
        f = fiber_create(entry, user_data, g_scheduler->config.stack_size);
    }

    if (!f) {
        return 0;
    }

    /* Lock-free atomic increment for spawn tracking */
    scheduler_atomic_inc_fibers_spawned();
    /* Increment atomic task count - this is what sync() waits on */
    scheduler_atomic_inc_task_count();

    /* Atomic round-robin worker selection */
    size_t worker_idx = atomic_fetch_add(&g_scheduler->next_worker, 1) % g_scheduler->num_workers;
    int worker_id = (int)worker_idx;

    /* Sharded counter increment - uses worker_id for low contention */
    scheduler_sharded_inc_task_count((uint32_t)worker_id);

    scheduler_schedule(f, worker_id);

    return fiber_id(f);
}

void scheduler_schedule(fiber_t* f, int worker_id) {
    if (!g_scheduler || !f) {
        return;
    }

    if (worker_id < 0 || worker_id >= (int)g_scheduler->num_workers) {
        pthread_mutex_lock(&g_scheduler->mutex);
        f->next_ready = g_scheduler->ready_queue;
        g_scheduler->ready_queue = f;
        pthread_cond_broadcast(&g_scheduler->cond);  // Use broadcast to ensure all workers wake up
        pthread_mutex_unlock(&g_scheduler->mutex);
    } else {
        worker_t* w = &g_scheduler->workers[worker_id];
        push_local(w, f);
        /* Broadcast to ensure worker wakes up */
        pthread_cond_broadcast(&g_scheduler->cond);
    }
}

/* ============================================ */
/* Batch Scheduling Implementation              */
/* ============================================ */

spawn_batch_t* scheduler_create_spawn_batch(size_t initial_capacity) {
    if (initial_capacity == 0) {
        initial_capacity = 16;
    }
    
    spawn_batch_t* batch = (spawn_batch_t*)calloc(1, sizeof(spawn_batch_t));
    if (!batch) {
        return NULL;
    }
    
    batch->fibers = (fiber_t**)calloc(initial_capacity, sizeof(fiber_t*));
    if (!batch->fibers) {
        free(batch);
        return NULL;
    }
    
    batch->capacity = initial_capacity;
    batch->count = 0;
    
    return batch;
}

void scheduler_destroy_spawn_batch(spawn_batch_t* batch) {
    if (!batch) {
        return;
    }
    
    /* Free any fibers that were added but not submitted */
    for (size_t i = 0; i < batch->count; i++) {
        if (batch->fibers[i]) {
            if (batch->fibers[i]->pool) {
                fiber_pool_free(batch->fibers[i]->pool, batch->fibers[i]);
            } else {
                fiber_free(batch->fibers[i]);
            }
        }
    }
    
    free(batch->fibers);
    free(batch);
}

int scheduler_spawn_batch_add(spawn_batch_t* batch, void (*entry)(void*), void* user_data) {
    if (!batch || !entry || !g_scheduler) {
        return -1;
    }
    
    /* Grow capacity if needed */
    if (batch->count >= batch->capacity) {
        size_t new_capacity = batch->capacity * 2;
        fiber_t** new_fibers = (fiber_t**)realloc(batch->fibers, new_capacity * sizeof(fiber_t*));
        if (!new_fibers) {
            return -1;
        }
        batch->fibers = new_fibers;
        batch->capacity = new_capacity;
    }
    
    /* Allocate fiber from pool */
    fiber_t* f = NULL;
    if (g_scheduler->fiber_pool) {
        f = fiber_pool_alloc((fiber_pool_t*)g_scheduler->fiber_pool);
        if (f) {
            f->func = entry;
            f->arg = user_data;
            f->parent = fiber_current();
            
            /* Lazy stack allocation */
            if (!f->stack_base) {
                size_t stack_size = g_scheduler->config.stack_size > 0 ? 
                    g_scheduler->config.stack_size : FIBER_DEFAULT_STACK_SIZE;
                f->stack_base = mmap(
                    NULL,
                    stack_size + 4096,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1,
                    0
                );
                if (f->stack_base == MAP_FAILED) {
                    fiber_pool_free((fiber_pool_t*)g_scheduler->fiber_pool, f);
                    return -1;
                }
                mprotect(f->stack_base, 4096, PROT_NONE);
                f->stack_size = stack_size;
                f->stack_capacity = stack_size;
                f->stack_ptr = (char*)f->stack_base + stack_size + 4096;
            }
        }
    }
    
    /* Fall back to direct allocation */
    if (!f) {
        f = fiber_create(entry, user_data, g_scheduler->config.stack_size);
    }
    
    if (!f) {
        return -1;
    }
    
    batch->fibers[batch->count++] = f;
    g_scheduler->stats.total_fibers_created++;
    
    return 0;
}

void scheduler_spawn_batch_submit(spawn_batch_t* batch) {
    if (!batch || batch->count == 0 || !g_scheduler) {
        return;
    }
    
    scheduler_t* sched = g_scheduler;
    
    /* Submit all fibers with a single lock acquisition */
    pthread_mutex_lock(&sched->mutex);
    
    for (size_t i = 0; i < batch->count; i++) {
        fiber_t* f = batch->fibers[i];
        if (!f) continue;
        
        /* Atomic round-robin worker selection */
        size_t worker_idx = atomic_fetch_add(&sched->next_worker, 1) % sched->num_workers;
        int worker_id = (int)worker_idx;
        
        worker_t* w = &sched->workers[worker_id];
        push_local(w, f);
    }
    
    pthread_cond_broadcast(&sched->cond);
    pthread_mutex_unlock(&sched->mutex);
    
    /* Reset batch */
    batch->count = 0;
}

void scheduler_block(void* reason) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        return;
    }
    
    fiber_t* f = fiber_current();
    if (!f) {
        return;
    }
    
    f->state = FIBER_WAITING;
    
    pthread_mutex_lock(&sched->mutex);
    f->next_ready = sched->blocked_queue;
    sched->blocked_queue = f;
    pthread_mutex_unlock(&sched->mutex);
    
    fiber_yield();
}

void scheduler_unblock(fiber_t* f) {
    if (!g_scheduler || !f) {
        return;
    }
    
    pthread_mutex_lock(&g_scheduler->mutex);
    
    fiber_t* prev = NULL;
    fiber_t* curr = g_scheduler->blocked_queue;
    while (curr) {
        if (curr == f) {
            if (prev) {
                prev->next_ready = curr->next_ready;
            } else {
                g_scheduler->blocked_queue = curr->next_ready;
            }
            break;
        }
        prev = curr;
        curr = curr->next_ready;
    }
    
    pthread_mutex_unlock(&g_scheduler->mutex);
    
    f->state = FIBER_READY;
    scheduler_schedule(f, -1);
}

void scheduler_yield(void) {
    fiber_yield();
}

void scheduler_wait(fiber_t* f) {
    if (!f) {
        return;
    }
    
    while (fiber_state(f) != FIBER_COMPLETED) {
        fiber_yield();
    }
}

/* Forward declaration for deque_empty */
static bool deque_empty(deque_t* dq);

void scheduler_wait_all(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        return;
    }

    /* Wait using atomic task count - this is the authoritative count */
    uint64_t spin_count = 0;
    while (scheduler_atomic_get_task_count() > 0) {
        /* First check if any workers are actually running */
        bool any_running = false;
        for (size_t i = 0; i < sched->num_workers; i++) {
            if (atomic_load(&sched->workers[i].running) && 
                !atomic_load(&sched->workers[i].stopped)) {
                any_running = true;
                break;
            }
        }
        
        if (!any_running) {
            /* Workers not running - use scheduler_run on main thread */
            fiber_t* f = NULL;
            
            /* Check global queue */
            pthread_mutex_lock(&sched->mutex);
            f = sched->ready_queue;
            if (f) {
                sched->ready_queue = f->next_ready;
            }
            pthread_mutex_unlock(&sched->mutex);
            
            if (f && (f->state == FIBER_NEW || f->state == FIBER_READY)) {
                /* Execute fiber directly on main thread */
                if (f->state == FIBER_NEW) {
                    if (setjmp(f->context) == 0) {
                        f->state = FIBER_RUNNING;
                        f->func(f->arg);
                        f->state = FIBER_COMPLETED;
                        scheduler_atomic_dec_task_count();
                        
                        if (f->pool) {
                            fiber_pool_free(f->pool, f);
                        } else {
                            fiber_free(f);
                        }
                    }
                }
                continue;
            }
            
            /* No work on global queue either - break to avoid infinite loop */
            break;
        }
        
        /* Yield to allow workers to run */
        sched_yield();
        
        /* After many spins, check for stuck state */
        spin_count++;
        if (spin_count > 1000000) {
            /* We've spun for a while - print diagnostic and continue */
            fprintf(stderr, "[gsyncio] scheduler_wait_all: spun %lu times, task_count=%lu\n", 
                    spin_count, scheduler_atomic_get_task_count());
            sched_yield();
            spin_count = 0;
        }
    }
}

/* Debug/Diagnostic functions implementation */

bool scheduler_workers_running(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        return false;
    }
    
    for (size_t i = 0; i < sched->num_workers; i++) {
        if (atomic_load(&sched->workers[i].running) && 
            !atomic_load(&sched->workers[i].stopped)) {
            return true;
        }
    }
    return false;
}

size_t scheduler_total_queued_fibers(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        return 0;
    }
    
    size_t total = 0;
    
    /* Count global queue */
    pthread_mutex_lock(&sched->mutex);
    fiber_t* f = sched->ready_queue;
    while (f) {
        total++;
        f = f->next_ready;
    }
    pthread_mutex_unlock(&sched->mutex);
    
    /* Count per-worker local queues */
    for (size_t i = 0; i < sched->num_workers; i++) {
        if (!deque_empty(sched->workers[i].deque)) {
            total += deque_size(sched->workers[i].deque);
        }
    }
    
    return total;
}

void scheduler_print_debug_info(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        fprintf(stderr, "[gsyncio] scheduler not initialized\n");
        return;
    }
    
    fprintf(stderr, "=== gsyncio Scheduler Debug Info ===\n");
    fprintf(stderr, "Workers: %zu\n", sched->num_workers);
    fprintf(stderr, "Running: %s\n", sched->running ? "yes" : "no");
    fprintf(stderr, "Task count (atomic): %lu\n", scheduler_atomic_get_task_count());
    fprintf(stderr, "Total queued fibers: %zu\n", scheduler_total_queued_fibers());
    fprintf(stderr, "Fibers spawned: %lu\n", sched->stats.atomic_fibers_spawned);
    fprintf(stderr, "Fibers completed: %lu\n", sched->stats.atomic_fibers_completed);
    
    for (size_t i = 0; i < sched->num_workers; i++) {
        worker_t* w = &sched->workers[i];
        fprintf(stderr, "  Worker %zu: running=%s, started=%s, stopped=%s, tasks=%lu, queue_size=%zu\n",
                i,
                atomic_load(&w->running) ? "yes" : "no",
                atomic_load(&w->started) ? "yes" : "no",
                atomic_load(&w->stopped) ? "yes" : "no",
                w->tasks_executed,
                deque_size(w->deque));
    }
    fprintf(stderr, "=====================================\n");
}

void scheduler_get_stats(scheduler_stats_t* stats) {
    if (!g_scheduler || !stats) {
        return;
    }
    *stats = g_scheduler->stats;
}

int scheduler_current_worker(void) {
    if (!g_scheduler) {
        return -1;
    }
    
    fiber_t* current = fiber_current();
    if (!current) {
        return -1;
    }
    
    for (size_t i = 0; i < g_scheduler->num_workers; i++) {
        if (g_scheduler->workers[i].current_fiber == current) {
            return (int)i;
        }
    }
    
    return -1;
}

size_t scheduler_num_workers(void) {
    return g_scheduler ? g_scheduler->num_workers : 0;
}

void scheduler_run(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        return;
    }
    
    while (sched->running) {
        fiber_t* f = pop_local(&sched->workers[0]);
        
        if (!f) {
            pthread_mutex_lock(&sched->mutex);
            f = sched->ready_queue;
            if (f) {
                sched->ready_queue = f->next_ready;
            }
            pthread_mutex_unlock(&sched->mutex);
        }
        
        if (f) {
            if (f->state == FIBER_NEW || f->state == FIBER_READY) {
                if (f->state == FIBER_NEW) {
                    if (setjmp(f->context) == 0) {
                        f->state = FIBER_RUNNING;
                        f->func(f->arg);

                        f->state = FIBER_COMPLETED;
                        sched->stats.total_fibers_completed++;
                        
                        /* Decrement global atomic task count */
                        scheduler_atomic_dec_task_count();
                        /* Sharded counter decrement */
                        scheduler_sharded_dec_task_count(scheduler_get_current_worker_id());

                        if (f->pool) {
                            fiber_pool_free(f->pool, f);
                        } else {
                            fiber_free(f);
                        }

                        continue;
                    }
                } else {
                    f->state = FIBER_RUNNING;
                    longjmp(f->context, 1);
                }
            }
        } else {
            if (sched->io_uring_enabled) {
                io_uring_submit(&sched->io_uring_ring);
                process_io_completions(sched);
            }
            
            bool has_work = false;
            for (size_t i = 0; i < sched->num_workers; i++) {
                if (!deque_empty(sched->workers[i].deque)) {
                    has_work = true;
                    break;
                }
            }
            
            if (!has_work && !sched->ready_queue && !sched->blocked_queue) {
                break;
            }
            
            fiber_yield();
        }
    }
}

void scheduler_stop(void) {
    if (g_scheduler) {
        g_scheduler->running = false;
    }
}

void scheduler_set_backend(scheduler_backend_t backend) {
    if (!g_scheduler) {
        return;
    }
    
#ifdef __linux__
    if (backend == SCHEDULER_BACKEND_IOURING && !g_scheduler->io_uring_enabled) {
        if (io_uring_init(&g_scheduler->io_uring_ring, 256) == 0) {
            g_scheduler->io_uring_enabled = true;
            g_scheduler->backend = SCHEDULER_BACKEND_IOURING;
        }
    } else if (backend == SCHEDULER_BACKEND_EPOLL) {
        if (g_scheduler->io_uring_enabled) {
            io_uring_destroy(&g_scheduler->io_uring_ring);
            g_scheduler->io_uring_enabled = false;
        }
        g_scheduler->backend = SCHEDULER_BACKEND_EPOLL;
    }
#else
    (void)backend;
#endif
}

scheduler_backend_t scheduler_get_backend(void) {
    return g_scheduler ? g_scheduler->backend : SCHEDULER_BACKEND_DEFAULT;
}

int scheduler_submit_io(io_request_t *req) {
    if (!g_scheduler || !req) {
        return -1;
    }
    
#ifdef __linux__
    if (g_scheduler->io_uring_enabled) {
        io_uring_submission_t *sub = (io_uring_submission_t*)malloc(sizeof(io_uring_submission_t));
        if (!sub) return -1;
        
        sub->user_data = req->user_data;
        sub->op = req->op;
        sub->fd = req->fd;
        sub->buf = req->buf;
        sub->len = req->len;
        sub->offset = req->offset;
        sub->fiber = req->fiber;
        
        pthread_mutex_lock(&g_scheduler->io_uring_mutex);
        sub->next = g_scheduler->pending_submissions;
        g_scheduler->pending_submissions = sub;
        pthread_mutex_unlock(&g_scheduler->io_uring_mutex);
        
        struct io_uring_sqe *sqe = io_uring_get_sqe(&g_scheduler->io_uring_ring);
        if (!sqe) {
            return -1;
        }
        
        switch (req->op) {
            case IO_OP_READ:
                sqe->opcode = IORING_OP_READ;
                sqe->addr = (uint64_t)req->buf;
                sqe->len = req->len;
                sqe->off = req->offset;
                break;
            case IO_OP_WRITE:
                sqe->opcode = IORING_OP_WRITE;
                sqe->addr = (uint64_t)req->write_buf;
                sqe->len = req->len;
                sqe->off = req->offset;
                break;
            case IO_OP_ACCEPT:
                sqe->opcode = IORING_OP_ACCEPT;
                sqe->addr = (uint64_t)req->addr;
                sqe->len = req->addrlen;
                break;
            case IO_OP_CONNECT:
                sqe->opcode = IORING_OP_CONNECT;
                sqe->addr = (uint64_t)req->addr;
                sqe->len = req->addrlen;
                break;
            default:
                sqe->opcode = IORING_OP_NOP;
                break;
        }
        
        sqe->fd = req->fd;
        sqe->user_data = req->user_data;
        
        g_scheduler->stats.total_io_submitted++;
        return 0;
    }
#endif
    
    (void)req;
    return -1;
}

int scheduler_wait_io(int fd, uint32_t events, int64_t timeout_ns) {
    if (!g_scheduler) {
        return -1;
    }
    
    fiber_t *fiber = fiber_current();
    if (!fiber) {
        return -1;
    }
    
    if (fd < 0 || fd >= (int)g_scheduler->fd_table_size) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler->pollers_mutex);
    
    io_poller_t *poller = (io_poller_t*)malloc(sizeof(io_poller_t));
    if (!poller) {
        pthread_mutex_unlock(&g_scheduler->pollers_mutex);
        return -1;
    }
    
    poller->fd = fd;
    poller->events = events;
    poller->waiting_fiber = fiber;
    poller->next = g_scheduler->pollers;
    g_scheduler->pollers = poller;
    
    pthread_mutex_unlock(&g_scheduler->pollers_mutex);
    
    g_scheduler->fd_table[fd].fiber = fiber;
    g_scheduler->fd_table[fd].events = events;
    g_scheduler->fd_table[fd].active = true;
    
    fiber->state = FIBER_WAITING;
    fiber->waiting_on = poller;
    
    fiber_yield();
    
    pthread_mutex_lock(&g_scheduler->pollers_mutex);
    io_poller_t **prev = &g_scheduler->pollers;
    io_poller_t *p = g_scheduler->pollers;
    while (p) {
        if (p == poller) {
            *prev = p->next;
            free(poller);
            break;
        }
        prev = &p->next;
        p = p->next;
    }
    pthread_mutex_unlock(&g_scheduler->pollers_mutex);
    
    g_scheduler->fd_table[fd].active = false;
    g_scheduler->fd_table[fd].fiber = NULL;
    
    return 0;
}

void scheduler_wake_io(int fd, uint32_t events) {
    if (!g_scheduler) {
        return;
    }
    
    if (fd < 0 || fd >= (int)g_scheduler->fd_table_size) {
        return;
    }
    
    fd_entry_t *entry = &g_scheduler->fd_table[fd];
    if (!entry->active || !entry->fiber) {
        return;
    }
    
    if ((events & EPOLLIN && (entry->events & EVLOOP_READ)) ||
        (events & EPOLLOUT && (entry->events & EVLOOP_WRITE))) {
        entry->active = false;
        
        fiber_t *fiber = entry->fiber;
        fiber->state = FIBER_READY;
        fiber->waiting_on = NULL;
        
        scheduler_schedule(fiber, -1);
    }
}

int scheduler_add_timer(uint64_t deadline_ns, fiber_t *fiber) {
    if (!g_scheduler || !fiber) {
        return -1;
    }
    
    timer_node_t *node = (timer_node_t*)malloc(sizeof(timer_node_t));
    if (!node) return -1;
    
    node->deadline_ns = deadline_ns;
    node->fiber = fiber;
    node->active = true;
    
    pthread_mutex_lock(&g_scheduler->timers_mutex);
    node->next = g_scheduler->timers;
    g_scheduler->timers = node;
    pthread_mutex_unlock(&g_scheduler->timers_mutex);
    
    return 0;
}

void scheduler_cancel_timer(fiber_t *fiber) {
    if (!g_scheduler || !fiber) {
        return;
    }
    
    pthread_mutex_lock(&g_scheduler->timers_mutex);
    timer_node_t *node = g_scheduler->timers;
    while (node) {
        if (node->fiber == fiber) {
            node->active = false;
            break;
        }
        node = node->next;
    }
    pthread_mutex_unlock(&g_scheduler->timers_mutex);
}

int scheduler_register_fd(int fd, fiber_t *fiber, uint32_t events) {
    if (!g_scheduler || fd < 0 || fd >= (int)g_scheduler->fd_table_size) {
        return -1;
    }
    
    g_scheduler->fd_table[fd].fiber = fiber;
    g_scheduler->fd_table[fd].events = events;
    g_scheduler->fd_table[fd].active = true;
    
    return 0;
}

void scheduler_unregister_fd(int fd) {
    if (!g_scheduler || fd < 0 || fd >= (int)g_scheduler->fd_table_size) {
        return;
    }
    
    g_scheduler->fd_table[fd].active = false;
    g_scheduler->fd_table[fd].fiber = NULL;
}
/* ============================================ */
/* Worker Manager Integration                   */
/* ============================================ */

/**
 * Background thread for worker scaling decisions
 */
static void* worker_manager_loop(scheduler_t *sched) {
    while (sched->running) {
        /* Check if scaling is needed */
        size_t queue_depth = 0;
        
        pthread_mutex_lock(&sched->mutex);
        fiber_t* f = sched->ready_queue;
        while (f) {
            queue_depth++;
            f = f->next_ready;
            if (queue_depth > 1000) break;  /* Cap counting */
        }
        pthread_mutex_unlock(&sched->mutex);
        
        /* Check scaling */
        worker_manager_check_scale(&sched->worker_manager, queue_depth);
        
        /* Sleep for check interval */
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = WORKER_MANAGER_CHECK_INTERVAL_MS * 1000000;
        nanosleep(&ts, NULL);
    }
    
    return NULL;
}

void scheduler_check_worker_scaling(void) {
    if (!g_scheduler) return;
    
    size_t queue_depth = 0;
    pthread_mutex_lock(&g_scheduler->mutex);
    fiber_t* f = g_scheduler->ready_queue;
    while (f) {
        queue_depth++;
        f = f->next_ready;
        if (queue_depth > 1000) break;
    }
    pthread_mutex_unlock(&g_scheduler->mutex);
    
    worker_manager_check_scale(&g_scheduler->worker_manager, queue_depth);
}

void scheduler_set_auto_scaling(bool enabled) {
    if (!g_scheduler) return;
    worker_manager_set_auto_scaling(&g_scheduler->worker_manager, enabled);
}

void scheduler_set_energy_efficient_mode(bool enabled) {
    if (!g_scheduler) return;
    worker_manager_set_energy_efficient_mode(&g_scheduler->worker_manager, enabled);
}

double scheduler_get_worker_utilization(void) {
    if (!g_scheduler) return 0.0;
    return worker_manager_get_utilization(&g_scheduler->worker_manager);
}

size_t scheduler_get_recommended_workers(void) {
    if (!g_scheduler) return WORKER_MANAGER_MIN_WORKERS;
    return worker_manager_get_recommended_workers(&g_scheduler->worker_manager);
}

/* ============================================ */
/* Batch Python Task Spawning (Lock-Free)      */
/* ============================================ */

/**
 * Spawn multiple Python tasks in a batch with minimal overhead.
 * Uses single lock acquisition for all spawns.
 *
 * Note: This function allocates fibers efficiently but the actual
 * Python callback is handled by the existing scheduler_spawn mechanism.
 *
 * @param tasks Array of python_task_t (func, args, fiber_id)
 * @param count Number of tasks
 * @return 0 on success, -1 on failure
 */
int scheduler_spawn_batch_python(python_task_t* tasks, size_t count) {
    /* Note: This function is a placeholder for future optimization.
     * Currently, batch spawning is handled more efficiently from Python
     * using spawn_batch() which reuses pooled payloads.
     * 
     * The C-level batch spawn would need proper Python callback integration
     * which requires careful GIL management.
     */
    (void)tasks;
    (void)count;
    return -1;  /* Not implemented - use Python spawn_batch() instead */
}
