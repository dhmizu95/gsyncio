/**
 * scheduler.c - M:N work-stealing scheduler implementation for gsyncio
 *
 * High-performance M:N scheduler that maps M fibers onto N worker threads
 * with work-stealing for load balancing. Includes io_uring integration.
 */

#define _GNU_SOURCE  /* For CPU_ZERO, CPU_SET, pthread_setaffinity_np */

/* Python.h first, as CPython requires. The worker loop holds the GIL
 * across a run of Python fibers (see GIL_RUN_MAX), which needs
 * PyGILState_Ensure/Release. Note this header was previously absent, so
 * the `#ifdef Py_BEGIN_ALLOW_THREADS` blocks in this file were compiling
 * to nothing at all. */
#include <Python.h>

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
    _Atomic(timer_node_t*) free_list;
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
        g_timer_pool.nodes[i].next = (timer_node_t*)atomic_load(&g_timer_pool.free_list);
        atomic_store(&g_timer_pool.free_list, &g_timer_pool.nodes[i]);
    }
}

static void timer_pool_free(timer_node_t* node) {
    if (!node) return;

    /* Check if node is from pool */
    if (node >= g_timer_pool.nodes && node < g_timer_pool.nodes + g_timer_pool.capacity) {
        timer_node_t* old_head;
        do {
            old_head = (timer_node_t*)atomic_load(&g_timer_pool.free_list);
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
    uint64_t remaining = __atomic_sub_fetch(&g_scheduler->stats.atomic_task_count, 1,
                                            __ATOMIC_SEQ_CST);
    if (remaining == 0) {
        /* Only on the last completion, so waiters (gs.sync()) can sleep
         * on a condvar rather than polling. Taking the mutex here is
         * what makes the waiter's "re-check under the lock, then wait"
         * sequence race-free. */
        pthread_mutex_lock(&g_scheduler->done_mutex);
        pthread_cond_broadcast(&g_scheduler->done_cond);
        pthread_mutex_unlock(&g_scheduler->done_mutex);
    }
    return remaining;
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
static inline uint64_t worker_rand(worker_t* w);

/* How many random victims an idle worker probes per steal round. */
#define STEAL_SAMPLES 4

/* Python fibers run back-to-back under one GIL acquisition, up to this
 * many, before the worker drops the GIL and re-takes it. */
#define GIL_RUN_MAX 128

/* Idle backoff bounds for a worker that keeps finding no work. */
#define IDLE_SLEEP_MIN_NS 1000000ULL    /* 1 ms  */
#define IDLE_SLEEP_MAX_NS 20000000ULL   /* 20 ms */

/* How long a worker spins watching its queues before parking.
 *
 * Parking is a futex round trip, so a worker that parks between two
 * tasks arriving 6 us apart pays that syscall on every task - measured
 * as a 18x regression on the nogil path (0.5 -> 9.4 us/task) when this
 * was ~100. It has to exceed the inter-arrival gap of a producer feeding
 * work round-robin to every worker. The cost of guessing high is only
 * spinning: a worker that stays idle still ends up in the backoff ladder
 * below, so a genuinely unused pool settles at roughly this spin per
 * IDLE_SLEEP_MAX_NS nap - about 1% of a core each. */
#define IDLE_SPIN_ITERS 4000

/* Hint to the CPU that this is a spin-wait: lowers power draw and stops
 * the spinning core from starving a sibling hyperthread that is doing
 * real work. Falls back to a plain compiler barrier elsewhere. */
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

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
    pthread_spin_init(&dq->resize_lock, PTHREAD_PROCESS_PRIVATE);
    return 0;
}

static void push_top(deque_t* dq, fiber_t* f) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    /* Check if we need to resize */
    if (b - t >= dq->capacity) {
        size_t old_capacity = dq->capacity;
        size_t new_capacity = old_capacity * 2;

        /* dq->data/dq->capacity are read by pop_top()/steal_bottom() on
         * other threads without any lock of their own - this spinlock is
         * the only thing making a resize safe against them (they take it
         * too, see below). A plain realloc() is not enough on its own:
         * the buffer is a circular ring indexed by (i & (capacity-1)), so
         * if the live [t, b) window has wrapped around the end of the
         * old array, growing the mask reshuffles which slot each index
         * maps to. Elements have to be copied out in logical order into
         * the new array under the *new* mask, not just byte-copied. */
        pthread_spin_lock(&dq->resize_lock);

        fiber_t** new_data = (fiber_t**)calloc(new_capacity, sizeof(fiber_t*));
        if (!new_data) {
            pthread_spin_unlock(&dq->resize_lock);
            return;
        }
        for (size_t i = t; i < b; i++) {
            new_data[i & (new_capacity - 1)] = dq->data[i & (old_capacity - 1)];
        }
        fiber_t** old_data = dq->data;
        dq->data = new_data;
        dq->capacity = new_capacity;

        pthread_spin_unlock(&dq->resize_lock);
        free(old_data);
    }

    /* Store fiber FIRST (before updating bottom) */
    dq->data[b & (dq->capacity - 1)] = f;

    /* Full memory barrier to ensure store is visible before bottom update */
    atomic_thread_fence(memory_order_seq_cst);

    /* Now increment bottom */
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
}

/* Simple deque operations (single-owner) */

static fiber_t* pop_top(deque_t* dq) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    if (t >= b) {
        return NULL;  /* Empty */
    }

    /* Snapshot data/capacity under the same lock push_top()'s resize uses -
     * push_top() may be growing the ring on another thread right now (the
     * push side runs on whichever thread called gs.task()/scheduler_spawn,
     * not necessarily this deque's own worker thread), and dq->data is a
     * plain pointer with no atomicity of its own. Uncontended in the
     * overwhelmingly common case (no resize in flight). */
    pthread_spin_lock(&dq->resize_lock);
    fiber_t* f = dq->data[t & (dq->capacity - 1)];
    pthread_spin_unlock(&dq->resize_lock);

    /* CAS to claim this slot: a concurrent steal_bottom() (or another
     * owner-side pop) may be racing for the same element. Without this,
     * two threads can both walk away with the same fiber pointer - one
     * runs it while the other frees/resets it underneath it. */
    if (!atomic_compare_exchange_strong_explicit(&dq->top, &t, t + 1,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        return NULL;  /* Lost the race */
    }
    return f;
}

static fiber_t* steal_bottom(deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);

    if (t >= b) {
        return NULL;  /* Empty */
    }

    /* See pop_top() - same resize race applies to thieves. */
    pthread_spin_lock(&dq->resize_lock);
    fiber_t* f = dq->data[t & (dq->capacity - 1)];
    pthread_spin_unlock(&dq->resize_lock);

    /* Same CAS as pop_top() - see comment there. */
    if (!atomic_compare_exchange_strong_explicit(&dq->top, &t, t + 1,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        return NULL;  /* Lost the race */
    }
    return f;
}

static bool deque_empty(deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    return t >= b;
}

/* Anything this worker could pick up right now, checked with plain
 * atomic loads (no locks) - used as the pre-park re-check. Deliberately
 * does NOT consider other workers' queues: missing a steal opportunity
 * only costs a wait, and the backoff timeout bounds it. */
static inline bool worker_has_work(worker_t* w, scheduler_t* sched) {
    if (w->gil_deque && !deque_empty(w->gil_deque)) return true;
    if (!deque_empty(w->deque)) return true;
    if (atomic_load_explicit(&sched->ready_queue, memory_order_relaxed)) return true;
    if (!w->gil_deque &&
        atomic_load_explicit(&sched->blocked_gil_workers,
                             memory_order_relaxed) > 0) return true;
    return false;
}

/* Book-keeping for a successful steal made outside steal_from_worker()
 * (which does its own accounting). */
static inline void thief_credit(worker_t* thief, scheduler_t* sched) {
    thief->steals_attempted++;
    thief->steals_successful++;
    sched->stats.total_work_steals++;
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
    
    /* Slow path: block this worker thread until the deadline.
     *
     * Fibers currently run as plain calls on the worker's own OS stack
     * (no stack-switching yet - see NATIVE_C_FIBERS_PLAN.md), so there is
     * no way to suspend mid-call and let the worker run other fibers.
     * A real OS sleep is the only correct wait here; other fibers still
     * make progress on the scheduler's other worker threads.
     */
    (void)current;
    scheduler_note_blocking_wait_begin();
    uint64_t deadline = get_time_ns() + ns;
    for (;;) {
        uint64_t now = get_time_ns();
        if (now >= deadline) {
            break;
        }
        uint64_t remaining = deadline - now;
        struct timespec req;
        req.tv_sec = (time_t)(remaining / 1000000000ULL);
        req.tv_nsec = (long)(remaining % 1000000000ULL);
        nanosleep(&req, NULL);
    }
    scheduler_note_blocking_wait_end();
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
        cpu_relax();
    }

    /* Consecutive rounds this worker has found nothing to do; drives the
     * idle backoff at the bottom of the loop. */
    unsigned idle_rounds = 0;

    /* GIL held across a run of Python fibers - see the acquire site. */
    PyGILState_STATE gil_state;
    int holding_gil = 0;
    unsigned gil_run = 0;

    while (w->running && !w->stopped) {
        /* Cap how long the GIL is held in one run, so a worker with a
         * deep queue of Python fibers still lets other Python threads
         * (including the one calling gs.task()) make progress. */
        if (holding_gil && gil_run >= GIL_RUN_MAX) {
            PyGILState_Release(gil_state);
            holding_gil = 0;
        }

        fiber_t* f = NULL;

        /* 0. GIL work first, on the workers that are allowed to run it.
         * Drained ahead of the local nogil deque so a GIL worker keeps
         * executing Python back-to-back under one GIL acquisition rather
         * than interleaving with nogil work it could have left to the
         * other workers. */
        if (w->gil_deque) {
            f = pop_top(w->gil_deque);
        }

        // 1. Try local queue first (fast path - no lock needed)
        if (!f) {
            f = pop_local(w);
        }

        /* 1b. A GIL worker balances against the *other* GIL workers
         * before touching nogil work. Only reachable with more than one
         * GIL worker (i.e. a free-threaded interpreter); with the default
         * single GIL worker this loop has no other queue to look at.
         * Note the plain deques below need no class check of their own:
         * scheduler_schedule() never puts a GIL-bound fiber on one. */
        if (!f && w->gil_deque && sched->config.work_stealing &&
            sched->num_gil_workers > 1) {
            for (size_t i = 1; i < sched->num_gil_workers; i++) {
                worker_t* victim =
                    &sched->workers[((size_t)w->id + i) % sched->num_gil_workers];
                if (!victim->gil_deque) continue;
                f = steal_bottom(victim->gil_deque);
                if (f) {
                    thief_credit(w, sched);
                    break;
                }
            }
        }

        // 2. Try adaptive work-stealing if no local work
        if (!f && sched->config.work_stealing) {
            int victim = select_victim_adaptive(w);
            if (victim >= 0) {
                f = steal_from_worker(w, victim);
            }

            /* Fallback: a few random probes. Deliberately NOT a scan of
             * every worker - that ran on every idle iteration of every
             * idle worker and cost more in cross-core traffic than the
             * work it found. Anything missed here is picked up on the
             * next loop, or by the condvar wake when work is pushed. */
            if (!f) {
                size_t n = sched->num_workers;
                for (int a = 0; a < STEAL_SAMPLES && !f; a++) {
                    int victim_id = (int)(worker_rand(w) % n);
                    if (victim_id == w->id) continue;
                    f = steal_from_worker(w, victim_id);
                }
            }
        }

        /* 2b. Stand in for a GIL worker that is parked in a blocking wait.
         * Only reached when this worker found nothing of its own to do,
         * and only while a GIL worker is actually blocked - so in the
         * normal case this costs one relaxed atomic load. Without it, a
         * single Python task that sleeps or waits on a Future would stall
         * every other Python task until it woke up, since by default
         * exactly one worker is allowed to run Python. */
        if (!f && !w->gil_deque &&
            atomic_load_explicit(&sched->blocked_gil_workers,
                                 memory_order_relaxed) > 0) {
            for (size_t i = 0; i < sched->num_gil_workers; i++) {
                worker_t* victim = &sched->workers[i];
                if (!victim->gil_deque) continue;
                f = steal_bottom(victim->gil_deque);
                if (f) {
                    thief_credit(w, sched);
                    break;
                }
            }
        }

        // Try global queue as last resort (lock-free pop)
        if (!f) {
            fiber_t* old_head = atomic_load(&sched->ready_queue);
            while (old_head != NULL) {
                fiber_t* next = old_head->next_ready;
                if (atomic_compare_exchange_weak(&sched->ready_queue, &old_head, next)) {
                    f = old_head;
                    break;
                }
                /* old_head updated by CAS failure */
            }
        }

        /* Nothing to run: give the GIL back before stealing, spinning or
         * parking. Holding it here would block every other Python thread
         * while this worker sits idle. */
        if (!f && holding_gil) {
            PyGILState_Release(gil_state);
            holding_gil = 0;
        }

        // 4. No work? Process timers and do a brief spin before sleeping
        if (!f) {
            process_timers(sched);

            /* Spin on cheap ATOMIC LOADS only - never on a locked pop.
             * The previous version called pop_local() (which takes the
             * deque's spinlock and runs a seq_cst CAS) up to 100 times
             * per idle round, on every idle worker. That is the single
             * biggest reason an otherwise idle pool burned most of the
             * machine: the idle workers were hammering the very locks
             * the busy worker needed. Here the loop only reads
             * top/bottom, and drops out to do a real pop when one of
             * them actually looks non-empty. */
            atomic_fetch_add(&sched->spinning_workers, 1);
            for (int spin = 0; spin < IDLE_SPIN_ITERS && !w->stopped; spin++) {
                cpu_relax();

                /* This worker's own deques live on cache lines nobody
                 * else writes in the common case, so polling them every
                 * iteration is nearly free. */
                if (w->gil_deque && !deque_empty(w->gil_deque)) {
                    f = pop_top(w->gil_deque);
                    if (f) break;
                }
                if (!deque_empty(w->deque)) {
                    f = pop_local(w);
                    if (f) break;
                }

                /* sched->ready_queue is written by every producer, so
                 * every worker polling it each iteration turns one cache
                 * line into an interconnect hot spot - with a dozen
                 * spinners that costs far more than the rare global-queue
                 * push it is watching for. Sample it occasionally
                 * instead; the park below re-checks it anyway. */
                if ((spin & 63) == 0 &&
                    atomic_load_explicit(&sched->ready_queue,
                                         memory_order_relaxed) != NULL) {
                    fiber_t* old_head = atomic_load(&sched->ready_queue);
                    while (old_head != NULL) {
                        fiber_t* next = old_head->next_ready;
                        if (atomic_compare_exchange_weak(&sched->ready_queue,
                                                         &old_head, next)) {
                            f = old_head;
                            break;
                        }
                    }
                    if (f) break;
                }

                /* Steal while spinning, not just after. This is what
                 * makes it safe for wake_worker() to skip the futex when
                 * somebody is already spinning: a spinner that only
                 * watched its own deques would never notice work pushed
                 * to a different worker, so the skipped wakeup would turn
                 * into a stall. Sampled, for the same cache-line reason
                 * as above. */
                if ((spin & 255) == 0 && sched->config.work_stealing) {
                    int victim_id = (int)(worker_rand(w) % sched->num_workers);
                    if (victim_id != w->id) {
                        f = steal_from_worker(w, victim_id);
                        if (f) break;
                    }
                }
            }
            atomic_fetch_sub(&sched->spinning_workers, 1);
        }

        // 5. Still no work? Wait on condition variable
        if (!f && !w->stopped) {
            /* Back off as idleness persists: a worker with nothing to do
             * should cost nothing. Starts at 1 ms so latency is unchanged
             * for a pool that is merely between tasks, and stretches to
             * IDLE_SLEEP_MAX_NS for one that is genuinely unused - which
             * is the normal state of the nogil workers when the program
             * only runs Python tasks. Reset to the floor the moment this
             * worker finds work again (see below). */
            idle_rounds++;
            uint64_t nap_ns = IDLE_SLEEP_MIN_NS << (idle_rounds > 5 ? 5 : idle_rounds - 1);
            if (nap_ns > IDLE_SLEEP_MAX_NS) {
                nap_ns = IDLE_SLEEP_MAX_NS;
            }

            struct timespec ts;

            pthread_mutex_lock(&w->park_mutex);
            atomic_store(&w->parked, 1);
            atomic_fetch_add(&sched->parked_workers, 1);

            /* Re-check AFTER publishing `parked`, under the same mutex a
             * waker must take. Either we see the work here, or the waker
             * sees parked==1 and signals - the window where both miss
             * each other is closed by this ordering. */
            if (!worker_has_work(w, sched)) {
                clock_gettime(CLOCK_REALTIME, &ts);
                uint64_t deadline = (uint64_t)ts.tv_sec * 1000000000ULL
                                    + (uint64_t)ts.tv_nsec + nap_ns;
                ts.tv_sec = (time_t)(deadline / 1000000000ULL);
                ts.tv_nsec = (long)(deadline % 1000000000ULL);
                pthread_cond_timedwait(&w->park_cond, &w->park_mutex, &ts);
            }

            atomic_fetch_sub(&sched->parked_workers, 1);
            atomic_store(&w->parked, 0);
            pthread_mutex_unlock(&w->park_mutex);
        }

        if (f) {
            idle_rounds = 0;
        }

        /* Hold the GIL across a RUN of Python fibers instead of letting
         * each one take and drop it.
         *
         * Every GIL-bound fiber body does PyGILState_Ensure/Release
         * internally (Cython's `with gil`). Those calls are cheap when
         * this thread already holds the GIL - they just nest - so
         * acquiring once here and running a run of queued Python fibers
         * under it removes an acquire/release pair per task. This is the
         * same amortisation spawn() gets by packing many calls into one
         * fiber, except it applies to gs.task(), where each task keeps
         * its own fiber and its own error isolation. Measured: per-task
         * GIL traffic was the gap between spawn()'s 0.07 us/task and
         * gs.task()'s ~10 us/task.
         *
         * Released before anything that blocks or idles (below), so this
         * never keeps the GIL from other Python threads while this
         * worker has nothing to run. */
        if (f && f->gil_bound && !holding_gil && !w->stopped) {
            gil_state = PyGILState_Ensure();
            holding_gil = 1;
            gil_run = 0;
        } else if (f && !f->gil_bound && holding_gil) {
            /* A nogil fiber has no business running under the GIL - it
             * would serialise against every Python thread for nothing. */
            PyGILState_Release(gil_state);
            holding_gil = 0;
        }
        if (holding_gil) {
            gil_run++;
        }

        // 6. Execute found fiber
        if (f && !w->stopped) {
            fiber_state_t expected_state = f->state;
            if (expected_state != FIBER_NEW && expected_state != FIBER_READY) {
                /* Fiber already being processed? Should not happen with deque/lock ownership */
                continue;
            }
            
            /* Atomically try to claim the fiber */
            if (!__atomic_compare_exchange_n(&f->state, &expected_state, FIBER_RUNNING,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                continue;
            }
            
            w->current_fiber = f;
            fiber_set_current(f);
            w->tasks_executed++;

            DEBUG_LOG_FIBER("Executing fiber", f);

            if (expected_state == FIBER_NEW) {
                DEBUG_LOG("Worker %d: Running NEW fiber %lu", w->id, (unsigned long)f->id);
                if (setjmp(f->context) == 0) {
                    f->func(f->arg);

                    /* Fiber completed - clean up */
                    f->state = FIBER_COMPLETED;
                    scheduler_atomic_inc_fibers_completed();
                    scheduler_atomic_dec_task_count();
                    scheduler_sharded_dec_task_count(scheduler_get_current_worker_id());

                    /* Deliberately NOT rescheduling f->parent here: fibers
                     * in this codebase run as plain calls on a worker's
                     * own OS thread (no real stack-switching), so a
                     * parent fiber that spawned this one may already be
                     * physically executing elsewhere (e.g. blocked in
                     * future_wait() waiting on a *different* child's
                     * result). Rescheduling it here races another worker
                     * thread against the parent's own still-live
                     * execution - two threads then contend for the same
                     * fiber's continuation, corrupting state (observed as
                     * a null f->func crash under nested create_task()
                     * calls). Nothing correctly depends on this: fibers
                     * that need to wait for a child already do so via
                     * Future.result() (future_wait() -> pthread_cond_wait,
                     * a real blocking wait), not by being "woken" via
                     * this parent-child link. */

                    if (f->pool) {
                        fiber_pool_free(f->pool, f);
                    } else {
                        fiber_free(f);
                    }
                    w->current_fiber = NULL;
                    fiber_set_current(NULL);
                    /* No sched_yield() here: it was a syscall on the
                     * completion path of every single task, and giving up
                     * the CPU right after finishing one task is the
                     * opposite of what this worker should do when its own
                     * queue still has work queued behind it. */
                    continue;
                }
                /* Fiber resumed here after yield (from a yield inside its own func) */
            } else {
                /* Resume existing fiber (FIBER_READY) */
                longjmp(f->context, 1);
            }

            /* After yield and eventual resumption, we reach here only if we fell through f->func(f->arg) or longjmp */
            /* But actually, for resumed fibers, they jump back to the setjmp ABOVE. */
            /* So this line is reached ONLY when a fiber yields and is then picked up again. */
            w->current_fiber = NULL;
            fiber_set_current(NULL);
        }
    }

    if (holding_gil) {
        PyGILState_Release(gil_state);
        holding_gil = 0;
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

/* Cheap per-worker PRNG (xorshift64*) for randomized victim choice. */
static inline uint64_t worker_rand(worker_t* w) {
    uint64_t x = w->rng_state;
    if (x == 0) {
        x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)w->id + 1);
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    w->rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Select victim based on load - steal from busiest worker.
 *
 * Samples a bounded number of RANDOM victims rather than scanning every
 * worker. The full scan this replaced was O(num_workers) cache-missing
 * loads of two hot atomics per idle iteration, run by every idle worker
 * in a tight loop - on 12 workers that is ~144 cross-core loads per
 * round, and it lands on exactly the deques that a busy worker is trying
 * to own. Randomized bounded sampling is what Go's scheduler does, for
 * the same reason. */
static int select_victim_adaptive(worker_t* thief) {
    scheduler_t* sched = g_scheduler;
    size_t n = sched->num_workers;
    if (n <= 1) {
        return -1;
    }

    size_t max_size = 0;
    int victim = -1;

    size_t samples = n - 1 < STEAL_SAMPLES ? n - 1 : STEAL_SAMPLES;
    for (size_t s = 0; s < samples; s++) {
        size_t i = (size_t)(worker_rand(thief) % n);
        if (i == (size_t)thief->id) {
            i = (i + 1) % n;
            if (i == (size_t)thief->id) continue;
        }

        size_t size = deque_size(sched->workers[i].deque);

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
        sched->config.max_fibers = 10000000;  /* 10M fibers default for high-concurrency */
        sched->config.stack_size = FIBER_DEFAULT_STACK_SIZE;
        sched->config.work_stealing = true;
        sched->config.backend = SCHEDULER_BACKEND_DEFAULT;
        sched->config.stack_mode = STACK_MODE_NATIVE;
        sched->config.io_uring_entries = 256;
    }

    sched->num_workers = sched->config.num_workers;
    sched->backend = sched->config.backend;

    /* Workers [0, num_gil_workers) get a gil_deque and are the only ones
     * that ever run Python. Default 1: under a normal (GIL-holding)
     * CPython, a second GIL worker turns every task boundary into a
     * cross-thread GIL handoff and makes things strictly worse. Clamped
     * to num_workers so a 1-worker scheduler still runs Python. */
    sched->num_gil_workers = sched->config.num_gil_workers;
    if (sched->num_gil_workers == 0) {
        sched->num_gil_workers = 1;
    }
    if (sched->num_gil_workers > sched->num_workers) {
        sched->num_gil_workers = sched->num_workers;
    }
    atomic_store(&sched->next_gil_worker, 0);

    /* Pre-allocate headroom for dynamic growth (see
     * scheduler_note_blocking_wait_begin()) so growth never has to
     * realloc `workers` - existing worker threads hold raw pointers
     * into this array (passed to pthread_create, tracked elsewhere),
     * and a realloc that moves the array would invalidate those
     * mid-flight. Only `num_workers` of these slots are actually
     * started below; the rest are zeroed and inert until grown into. */
    sched->workers_capacity = sched->num_workers * 16;
    sched->workers = (worker_t*)calloc(sched->workers_capacity, sizeof(worker_t));
    if (!sched->workers) {
        free(sched);
        return -1;
    }
    pthread_mutex_init(&sched->growth_mutex, NULL);
    atomic_store(&sched->blocked_workers, 0);

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

        pthread_mutex_init(&w->park_mutex, NULL);
        pthread_cond_init(&w->park_cond, NULL);
        atomic_store(&w->parked, 0);
        w->rng_state = 0;

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

        /* Only the GIL workers carry a second queue; gil_deque == NULL is
         * what identifies a worker as nogil-only everywhere else. */
        w->gil_deque = NULL;
        if (i < sched->num_gil_workers) {
            w->gil_deque = (deque_t*)calloc(1, sizeof(deque_t));
            if (!w->gil_deque || deque_init(w->gil_deque, 65536) != 0) {
                free(w->gil_deque);
                free(w->deque->data);
                free(w->deque);
                for (size_t j = 0; j < i; j++) {
                    if (sched->workers[j].gil_deque) {
                        free(sched->workers[j].gil_deque->data);
                        free(sched->workers[j].gil_deque);
                    }
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
    }
    
    pthread_mutex_init(&sched->mutex, NULL);
    pthread_cond_init(&sched->cond, NULL);
    pthread_mutex_init(&sched->done_mutex, NULL);
    pthread_cond_init(&sched->done_cond, NULL);

    /* Initialize worker manager */
    worker_manager_init(&sched->worker_manager, sched->num_workers);

    /* Initialize sharded counters for low-contention task counting */
    sharded_counter_init(&sched->sharded_task_count);
    sharded_counter_init(&sched->sharded_completion_count);

    /* Start with 8K fibers - grows on demand to 10M */
    sched->fiber_pool = fiber_pool_create(8192, sched->config.stack_mode);
    DEBUG_LOG("Fiber pool created: capacity=%zu", fiber_pool_capacity(sched->fiber_pool));

    /* Initialize timer pool for fast sleep allocation */
    timer_pool_init(131072);  /* Support up to 128K concurrent timers */
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

/* Assumes sched->growth_mutex is held. Starts one more worker thread in
 * the next unused pre-allocated slot, or does nothing if already at
 * workers_capacity (accepting the small residual risk of exhaustion at
 * extreme nesting depths rather than growing without bound). */
static void scheduler_grow_workers_locked(scheduler_t* sched) {
    if (sched->num_workers >= sched->workers_capacity) {
        return;
    }

    size_t i = sched->num_workers;
    worker_t* w = &sched->workers[i];
    w->id = (int)i;
    atomic_store(&w->running, false);
    atomic_store(&w->started, false);
    atomic_store(&w->stopped, false);
    w->current_fiber = NULL;
    w->tasks_executed = 0;
    w->steals_attempted = 0;
    w->steals_successful = 0;
    w->rng_state = 0;
    pthread_mutex_init(&w->park_mutex, NULL);
    pthread_cond_init(&w->park_cond, NULL);
    atomic_store(&w->parked, 0);
    /* Grown workers are nogil-only: they exist to absorb blocking waits,
     * and a GIL worker's queue is covered by the blocked_gil_workers
     * stand-in path instead. */
    w->gil_deque = NULL;

    w->deque = (deque_t*)calloc(1, sizeof(deque_t));
    if (!w->deque) {
        return;  /* Couldn't grow - caller proceeds without the extra worker */
    }
    if (deque_init(w->deque, 65536) != 0) {
        free(w->deque);
        w->deque = NULL;
        return;
    }

    atomic_store(&w->running, true);
    if (pthread_create(&w->thread, NULL, worker_thread, w) != 0) {
        atomic_store(&w->running, false);
        free(w->deque->data);
        free(w->deque);
        w->deque = NULL;
        return;
    }

#ifdef __linux__
    size_t num_cpus = get_num_cpus();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i % num_cpus, &cpuset);
    pthread_setaffinity_np(w->thread, sizeof(cpuset), &cpuset);
#endif

    /* Publish last: readers iterating 0..num_workers must never see this
     * slot before it's fully initialized and its thread is live. */
    sched->num_workers = i + 1;
    DEBUG_LOG("Grew worker pool to %zu workers (blocked-wait pressure)", sched->num_workers);
}

void scheduler_note_blocking_wait_begin(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched || !fiber_current()) {
        return;  /* Only worker threads occupy a pool slot */
    }

    /* If the parking thread is one of the (by default: one) workers
     * allowed to run Python, flag that the GIL queues need a stand-in,
     * or Python work would sit undrained until this wait returns. */
    int wid = scheduler_current_worker();
    if (wid >= 0 && (size_t)wid < sched->num_gil_workers) {
        atomic_fetch_add(&sched->blocked_gil_workers, 1);
    }

    size_t blocked = atomic_fetch_add(&sched->blocked_workers, 1) + 1;
    if (blocked >= sched->num_workers) {
        pthread_mutex_lock(&sched->growth_mutex);
        /* Re-check under the lock: another thread may have already grown
         * the pool between our check above and acquiring this lock. */
        if (atomic_load(&sched->blocked_workers) >= sched->num_workers) {
            scheduler_grow_workers_locked(sched);
        }
        pthread_mutex_unlock(&sched->growth_mutex);
    }
}

void scheduler_note_blocking_wait_end(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched || !fiber_current()) {
        return;
    }
    int wid = scheduler_current_worker();
    if (wid >= 0 && (size_t)wid < sched->num_gil_workers) {
        atomic_fetch_sub(&sched->blocked_gil_workers, 1);
    }
    atomic_fetch_sub(&sched->blocked_workers, 1);
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

    /* Workers park on their own condvar now, so the broadcast above is
     * not enough to reach them - without this, join() would stall for a
     * full idle-backoff interval per sleeping worker. */
    for (size_t i = 0; i < sched->num_workers; i++) {
        worker_t* w = &sched->workers[i];
        pthread_mutex_lock(&w->park_mutex);
        pthread_cond_broadcast(&w->park_cond);
        pthread_mutex_unlock(&w->park_mutex);
    }

    for (size_t i = 0; i < sched->num_workers; i++) {
        pthread_join(sched->workers[i].thread, NULL);
        if (sched->workers[i].deque) {
            free(sched->workers[i].deque->data);
            free(sched->workers[i].deque);
        }
        if (sched->workers[i].gil_deque) {
            free(sched->workers[i].gil_deque->data);
            free(sched->workers[i].gil_deque);
            sched->workers[i].gil_deque = NULL;
        }
        pthread_cond_destroy(&sched->workers[i].park_cond);
        pthread_mutex_destroy(&sched->workers[i].park_mutex);
    }

    pthread_mutex_destroy(&sched->mutex);
    pthread_cond_destroy(&sched->cond);
    pthread_mutex_destroy(&sched->done_mutex);
    pthread_cond_destroy(&sched->done_cond);
    pthread_mutex_destroy(&sched->growth_mutex);

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

size_t scheduler_num_gil_workers(void) {
    return g_scheduler ? g_scheduler->num_gil_workers : 0;
}

uint64_t scheduler_spawn(void (*entry)(void*), void* user_data) {
    /* Default is the nogil form: pure-C callers (c_tasks) keep using
     * every worker. Python bodies must go through scheduler_spawn_ex(). */
    return scheduler_spawn_ex(entry, user_data, 0);
}

uint64_t scheduler_spawn_ex(void (*entry)(void*), void* user_data, int gil_bound) {
    if (!g_scheduler || !entry) {
        return 0;
    }

    fiber_t* f = NULL;

    /* Round-robin worker selection - computed BEFORE pool allocation so
     * we can allocate from the shard matching the worker that will
     * actually run this fiber. Allocating from a mismatched shard
     * starves it and forces the pool to keep growing instead of
     * reusing freed fibers (see fiber_pool_alloc()'s doc comment).
     *
     * GIL-bound fibers round-robin only over the GIL workers (default:
     * just worker 0), so consecutive Python tasks stay on one OS thread
     * and run under a single GIL acquisition instead of handing the GIL
     * back and forth. nogil fibers still spread over every worker. */
    size_t worker_idx;
    if (gil_bound) {
        worker_idx = atomic_fetch_add(&g_scheduler->next_gil_worker, 1)
                     % g_scheduler->num_gil_workers;
    } else {
        worker_idx = atomic_fetch_add(&g_scheduler->next_worker, 1)
                     % g_scheduler->num_workers;
    }
    int worker_id = (int)worker_idx;

    /* Try fiber pool first for faster allocation */
    if (g_scheduler->fiber_pool) {
        f = fiber_pool_alloc((fiber_pool_t*)g_scheduler->fiber_pool, worker_id);
        if (f) {
            /* Initialize fiber fields */
            f->func = entry;
            f->arg = user_data;
            f->parent = fiber_current();

            /* Lazy stack allocation - allocate now if needed.
             * Compiled out by default: fiber bodies run as plain calls
             * on the worker's own stack, so this mmap was per-fiber
             * waste. See FIBER_ALLOCATE_STACKS in fiber.h. */
#if FIBER_ALLOCATE_STACKS == 1
            if (!f->stack_base) {
                size_t stack_size = g_scheduler->config.stack_size > 0 ?
                    g_scheduler->config.stack_size : FIBER_DEFAULT_STACK_SIZE;
                size_t alloc_size = stack_size;
#if FIBER_USE_GUARD_PAGES == 1
                alloc_size += 4096;
#endif
                f->stack_base = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                
                if (f->stack_base == MAP_FAILED) {
                    fiber_pool_free((fiber_pool_t*)g_scheduler->fiber_pool, f);
                    return 0;
                }
                
                f->mmap_size = alloc_size;
                
#if FIBER_USE_GUARD_PAGES == 1
                mprotect(f->stack_base, 4096, PROT_NONE);
                f->stack_ptr = (char*)f->stack_base + stack_size + 4096;
#else
                f->stack_ptr = (char*)f->stack_base + stack_size;
#endif
                f->stack_size = stack_size;
                f->stack_capacity = stack_size;
            }
#endif /* FIBER_ALLOCATE_STACKS */
        }
    }

    /* Fall back to direct allocation if pool exhausted */
    if (!f) {
        f = fiber_create(entry, user_data, g_scheduler->config.stack_size);
    }

    if (!f) {
        return 0;
    }

    /* Recorded on the fiber so a later reschedule (fiber_yield, unblock)
     * can route it back to a worker of the right class. */
    f->gil_bound = gil_bound ? 1 : 0;

    /* Lock-free atomic increment for spawn tracking */
    scheduler_atomic_inc_fibers_spawned();
    /* Increment atomic task count - this is what sync() waits on */
    scheduler_atomic_inc_task_count();

    /* Sharded counter increment - uses worker_id for low contention */
    scheduler_sharded_inc_task_count((uint32_t)worker_id);

    scheduler_schedule(f, worker_id);

    return fiber_id(f);
}

/* Wake one specific worker, and only if it is actually asleep.
 *
 * The relaxed load is the fast path: with a busy worker the answer is
 * "not parked" and this costs nothing, versus the unconditional
 * pthread_cond_signal() per spawn it replaces. Taking park_mutex when it
 * *is* parked is what closes the lost-wakeup window against the parker,
 * which sets `parked` and re-checks its queues under the same mutex. */
static inline void wake_worker(worker_t* w) {
    if (atomic_load_explicit(&w->parked, memory_order_relaxed)) {
        pthread_mutex_lock(&w->park_mutex);
        pthread_cond_signal(&w->park_cond);
        pthread_mutex_unlock(&w->park_mutex);
    }
}

/* Wake the owner of freshly pushed work - unless a worker is already
 * spinning, in which case skip the futex entirely and let it steal the
 * fiber. This is the common case for a producer feeding tasks faster
 * than any single worker drains them, where waking the target per task
 * costs a syscall per task. Correct because spinners steal (see the
 * spin loop), and because a parked worker still has its backoff timeout
 * as a backstop if the spinner happens to miss it. */
static inline void wake_owner(scheduler_t* sched, worker_t* w) {
    if (!atomic_load_explicit(&w->parked, memory_order_relaxed)) {
        return;  /* already awake - nothing to do */
    }
    if (atomic_load_explicit(&sched->spinning_workers, memory_order_relaxed) > 0) {
        return;  /* a spinner will pick it up */
    }
    wake_worker(w);
}

/* Wake any one sleeping worker - for pushes to the shared ready queue,
 * which has no particular owner. */
static void wake_any_worker(scheduler_t* sched) {
    if (atomic_load_explicit(&sched->parked_workers, memory_order_relaxed) == 0) {
        return;
    }
    for (size_t i = 0; i < sched->num_workers; i++) {
        if (atomic_load_explicit(&sched->workers[i].parked, memory_order_relaxed)) {
            wake_worker(&sched->workers[i]);
            return;
        }
    }
}

void scheduler_schedule(fiber_t* f, int worker_id) {
    if (!g_scheduler || !f) {
        return;
    }

    /* A GIL-bound fiber must never land on the global ready queue or on a
     * plain worker deque: both are drained by nogil workers, which would
     * put Python back on several OS threads at once and undo the whole
     * point of the split. Route it to a GIL worker's gil_deque instead,
     * picking one here if the caller didn't name a valid one. */
    if (f->gil_bound) {
        size_t g = g_scheduler->num_gil_workers;
        if (g == 0) {
            g = 1;  /* degenerate config - worker 0 still runs it */
        }
        size_t target;
        if (worker_id >= 0 && (size_t)worker_id < g) {
            target = (size_t)worker_id;
        } else {
            target = atomic_fetch_add(&g_scheduler->next_gil_worker, 1) % g;
        }
        worker_t* w = &g_scheduler->workers[target];
        push_top(w->gil_deque ? w->gil_deque : w->deque, f);
        wake_worker(w);
        return;
    }

    if (worker_id < 0 || worker_id >= (int)g_scheduler->num_workers) {
        /* Lock-free push to global ready queue */
        fiber_t* old_head = atomic_load(&g_scheduler->ready_queue);
        do {
            f->next_ready = old_head;
        } while (!atomic_compare_exchange_weak(&g_scheduler->ready_queue, &old_head, f));

        /* No particular owner - wake whoever is asleep */
        wake_any_worker(g_scheduler);
    } else {
        worker_t* w = &g_scheduler->workers[worker_id];
        push_local(w, f);
        /* Wake exactly the worker that now owns this fiber - or nobody,
         * if a spinner is already positioned to steal it. */
        wake_owner(g_scheduler, w);
    }
}

/* ============================================ */
/* Batch Scheduling Implementation              */
/* ============================================ */

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

    /* Blocks on done_cond, which the last completing task broadcasts.
     * The previous version polled the counter every millisecond, which
     * put a 1 ms floor under every gs.sync() no matter how quick the
     * work was, and burned a wakeup per millisecond while waiting.
     *
     * Callers reach here through Cython's `with nogil`, so the GIL is
     * already released - the Py_BEGIN_ALLOW_THREADS that used to wrap
     * the sleep here was releasing a lock this thread did not hold.
     *
     * The timed wait is a liveness backstop, not the mechanism: it
     * covers the case where the count reached zero between our check
     * and the wait despite the mutex, and bounds any lost wakeup. */
    if (scheduler_atomic_get_task_count() == 0) {
        return;
    }

    pthread_mutex_lock(&sched->done_mutex);
    while (scheduler_atomic_get_task_count() > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t deadline = (uint64_t)ts.tv_sec * 1000000000ULL
                            + (uint64_t)ts.tv_nsec + 20000000ULL; /* 20 ms */
        ts.tv_sec = (time_t)(deadline / 1000000000ULL);
        ts.tv_nsec = (long)(deadline % 1000000000ULL);
        pthread_cond_timedwait(&sched->done_cond, &sched->done_mutex, &ts);
    }
    pthread_mutex_unlock(&sched->done_mutex);
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
    
    /* Count per-worker local queues (both classes) */
    for (size_t i = 0; i < sched->num_workers; i++) {
        if (!deque_empty(sched->workers[i].deque)) {
            total += deque_size(sched->workers[i].deque);
        }
        if (sched->workers[i].gil_deque &&
            !deque_empty(sched->workers[i].gil_deque)) {
            total += deque_size(sched->workers[i].gil_deque);
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
    fprintf(stderr, "Fiber pool capacity (distinct fibers ever minted): %zu\n", fiber_pool_capacity((fiber_pool_t*)sched->fiber_pool));
    {
        size_t primary, fallback, grows;
        fiber_pool_diag_counts(&primary, &fallback, &grows);
        fprintf(stderr, "Alloc breakdown: primary_hit=%zu fallback_hit=%zu grow=%zu\n", primary, fallback, grows);
    }
    
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
            /* Check fiber state hasn't changed (prevent double execution) */
            fiber_state_t expected_state = f->state;
            if (expected_state != FIBER_NEW && expected_state != FIBER_READY) {
                /* Fiber already being processed */
                f = NULL;
            } else {
                /* Atomically try to claim the fiber */
                if (!__atomic_compare_exchange_n(&f->state, &expected_state, FIBER_RUNNING,
                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    /* Another worker claimed it first */
                    f = NULL;
                }
            }
            
            if (f && f->state == FIBER_RUNNING) {
                fiber_set_current(f);
                if (expected_state == FIBER_NEW) {
                    if (setjmp(f->context) == 0) {
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

                        fiber_set_current(NULL);
                        continue;
                    }
                    /* Fiber resumed after yield */
                } else {
                    /* Resume existing fiber at its yield point */
                    longjmp(f->context, 1);
                }
                fiber_set_current(NULL);
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

/* ============================================ */
/* Worker Manager Integration                   */
/* ============================================ */

/**
 * Background thread for worker scaling decisions
 */
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
