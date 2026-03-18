/**
 * scheduler.c - M:N work-stealing scheduler implementation for gsyncio
 * 
 * High-performance M:N scheduler that maps M fibers onto N worker threads
 * with work-stealing for load balancing. Includes io_uring integration.
 */

#include "scheduler.h"
#include "fiber_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#ifdef __linux__
#include "io_uring.h"
#endif

scheduler_t* g_scheduler = NULL;

static void* worker_thread(void* arg);
static fiber_t* steal_from_worker(worker_t* thief, int victim_id);
static void push_local(worker_t* w, fiber_t* f);
static fiber_t* pop_local(worker_t* w);
static void process_io_completions(scheduler_t *sched);

static size_t get_num_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (size_t)n : 1;
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
    if (dq->top >= dq->capacity) {
        size_t new_capacity = dq->capacity * 2;
        fiber_t** new_data = (fiber_t**)realloc(dq->data, new_capacity * sizeof(fiber_t*));
        if (!new_data) {
            return;
        }
        dq->data = new_data;
        dq->capacity = new_capacity;
    }
    dq->data[dq->top++] = f;
}

static fiber_t* pop_top(deque_t* dq) {
    if (dq->top == 0) {
        return NULL;
    }
    return dq->data[--dq->top];
}

static fiber_t* steal_bottom(deque_t* dq) {
    if (dq->bottom >= dq->top) {
        return NULL;
    }
    fiber_t* f = dq->data[dq->bottom];
    dq->bottom++;
    return f;
}

static bool deque_empty(deque_t* dq) {
    return dq->top <= dq->bottom;
}

static void process_io_completions(scheduler_t *sched) {
#ifdef __linux__
    if (!sched->io_uring_enabled) {
        return;
    }
    
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&sched->io_uring_ring, &cqe) == 1) {
        uint64_t user_data = cqe->user_data;
        int64_t result = cqe->res;
        
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

static void* worker_thread(void* arg) {
    worker_t* w = (worker_t*)arg;
    scheduler_t* sched = g_scheduler;
    
    // Warmup - do a quick spin to avoid cold-start latency
    for (int i = 0; i < 1000; i++) {
        __asm__ __volatile__("" ::: "memory");
    }
    
    while (w->running) {
        fiber_t* f = NULL;
        
        // Try local queue first (fast path)
        f = pop_local(w);
        
        // Try work stealing if no local work
        if (!f && sched->config.work_stealing) {
            for (size_t i = 0; i < sched->num_workers; i++) {
                int victim_id = (w->id + i + 1) % sched->num_workers;
                f = steal_from_worker(w, victim_id);
                if (f) break;
            }
        }
        
        // Try global queue
        if (!f) {
            pthread_mutex_lock(&sched->mutex);
            f = sched->ready_queue;
            if (f) {
                sched->ready_queue = f->next_ready;
            }
            pthread_mutex_unlock(&sched->mutex);
        }
        
        if (f) {
            w->current_fiber = f;
            w->tasks_executed++;
            
            if (f->state == FIBER_NEW || f->state == FIBER_READY) {
                sigjmp_buf jump_buf;
                f->sched_jump = &jump_buf;
                
                if (f->state == FIBER_NEW) {
                    if (sigsetjmp(f->context, 1) == 0) {
                        f->state = FIBER_RUNNING;
                        fiber_t* old_current = fiber_current();
                        f->func(f->arg);
                        
                        f->state = FIBER_COMPLETED;
                        sched->stats.total_fibers_completed++;
                        
                        if (f->parent) {
                            scheduler_schedule(f->parent, -1);
                        }
                        
                        if (f->pool) {
                            fiber_pool_free(f->pool, f);
                        } else {
                            fiber_free(f);
                        }
                        
                        w->current_fiber = NULL;
                        f->sched_jump = NULL;
                        continue;
                    }
                } else {
                    if (sigsetjmp(f->context, 1) == 0) {
                        f->state = FIBER_RUNNING;
                        siglongjmp(f->context, 1);
                    }
                }
                
                f->sched_jump = NULL;
            }
            
            w->current_fiber = NULL;
        } else {
            if (sched->io_uring_enabled) {
                io_uring_submit(&sched->io_uring_ring);
                process_io_completions(sched);
            }
            
            // Brief spin before sleeping to reduce wake-up latency
            for (int spin = 0; spin < 100 && !w->stopped; spin++) {
                __asm__ __volatile__("" ::: "memory");
                
                // Check queue during spin
                pthread_mutex_lock(&sched->mutex);
                fiber_t* f = sched->ready_queue;
                if (f) {
                    sched->ready_queue = f->next_ready;
                }
                pthread_mutex_unlock(&sched->mutex);
                if (f) break;
            }
            
            // Sleep with timeout instead of indefinite wait
            if (!w->stopped) {
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

static fiber_t* steal_from_worker(worker_t* thief, int victim_id) {
    scheduler_t* sched = g_scheduler;
    if (victim_id < 0 || victim_id >= (int)sched->num_workers) {
        return NULL;
    }
    
    worker_t* victim = &sched->workers[victim_id];
    deque_t* dq = victim->deque;
    if (dq->bottom >= dq->top) {
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
    
    static _Thread_local unsigned int seed = 0;
    if (seed == 0) {
        seed = (unsigned int)((uintptr_t)w ^ time(NULL));
    }
    
    int victim = w->last_victim;
    int attempts = 0;
    
    do {
        victim = (victim + 1 + (rand_r(&seed) % (sched->num_workers - 1))) % sched->num_workers;
        attempts++;
    } while (victim == w->id && attempts < sched->num_workers);
    
    w->last_victim = victim;
    return victim;
}

int scheduler_init(scheduler_config_t* config) {
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
        sched->config.max_fibers = 1000000;
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
        w->running = true;
        w->stopped = false;
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
        
        if (deque_init(w->deque, 1024) != 0) {
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
    
    sched->fiber_pool = fiber_pool_create(sched->config.max_fibers);
    
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
    
    for (size_t i = 0; i < sched->num_workers; i++) {
        pthread_create(&sched->workers[i].thread, NULL, worker_thread, &sched->workers[i]);
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
        sched->workers[i].stopped = true;
        sched->workers[i].running = false;
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
    
    fiber_pool_destroy(sched->fiber_pool);
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
    
    fiber_t* f = fiber_create(entry, user_data, g_scheduler->config.stack_size);
    if (!f) {
        return 0;
    }
    
    g_scheduler->stats.total_fibers_created++;
    
    int worker_id = g_scheduler->next_worker % g_scheduler->num_workers;
    g_scheduler->next_worker++;
    
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
        pthread_cond_signal(&g_scheduler->cond);
        pthread_mutex_unlock(&g_scheduler->mutex);
    } else {
        worker_t* w = &g_scheduler->workers[worker_id];
        push_local(w, f);
        pthread_cond_signal(&g_scheduler->cond);
    }
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

void scheduler_wait_all(void) {
    scheduler_t* sched = g_scheduler;
    if (!sched) {
        return;
    }
    
    while (sched->ready_queue || sched->blocked_queue) {
        bool has_work = false;
        for (size_t i = 0; i < sched->num_workers; i++) {
            if (sched->workers[i].current_fiber) {
                has_work = true;
                fiber_yield();
                break;
            }
        }
        
        if (!has_work) {
            break;
        }
    }
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
                sigjmp_buf jump_buf;
                f->sched_jump = &jump_buf;
                
                if (sigsetjmp(f->context, 1) == 0) {
                    f->state = FIBER_RUNNING;
                    f->func(f->arg);
                    
                    f->state = FIBER_COMPLETED;
                    sched->stats.total_fibers_completed++;
                    
                    if (f->pool) {
                        fiber_pool_free(f->pool, f);
                    } else {
                        fiber_free(f);
                    }
                    
                    f->sched_jump = NULL;
                    continue;
                }
                
                f->sched_jump = NULL;
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