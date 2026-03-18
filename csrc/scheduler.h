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
#include <sys/socket.h>

#ifdef __linux__
#include "io_uring.h"
#include "evloop.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCHEDULER_BACKEND_DEFAULT = 0,
    SCHEDULER_BACKEND_EPOLL = 1,
    SCHEDULER_BACKEND_IOURING = 2
} scheduler_backend_t;

typedef enum {
    IO_OP_READ = 0,
    IO_OP_WRITE = 1,
    IO_OP_ACCEPT = 2,
    IO_OP_CONNECT = 3,
    IO_OP_POLL_ADD = 4,
    IO_OP_SEND = 5,
    IO_OP_RECV = 6,
    IO_OP_TIMEOUT = 7
} io_operation_type_t;

typedef struct io_request io_request_t;

typedef void (*io_completion_callback_t)(io_request_t *req, int64_t result);

struct io_request {
    fiber_t *fiber;
    int fd;
    io_operation_type_t op;
    void *buf;
    const void *write_buf;
    size_t len;
    int64_t offset;
    struct sockaddr *addr;
    socklen_t addrlen;
    uint64_t user_data;
    io_completion_callback_t completion;
    void *completion_arg;
    int64_t result;
    bool completed;
    io_request_t *next;
};

typedef struct io_poller {
    int fd;
    uint32_t events;
    fiber_t *waiting_fiber;
    struct io_poller *next;
} io_poller_t;

typedef struct io_uring_submission io_uring_submission_t;

struct io_uring_submission {
    uint64_t user_data;
    io_operation_type_t op;
    int fd;
    void *buf;
    size_t len;
    int64_t offset;
    fiber_t *fiber;
    io_uring_submission_t *next;
};

typedef struct timer_node timer_node_t;

struct timer_node {
    uint64_t deadline_ns;
    fiber_t *fiber;
    timer_node_t *next;
    bool active;
};

typedef struct pending_io pending_io_t;

struct pending_io {
    int fd;
    uint32_t events;
    fiber_t *fiber;
    pending_io_t *next;
};

typedef struct io_bucket {
    pending_io_t *head;
    pending_io_t *tail;
    int count;
} io_bucket_t;

#define FD_TABLE_SIZE 65536

typedef struct {
    fiber_t *fiber;
    uint32_t events;
    bool active;
} fd_entry_t;

typedef struct deque {
    fiber_t** data;
    size_t capacity;
    size_t top;
    size_t bottom;
} deque_t;

typedef struct worker {
    int id;
    deque_t* deque;
    fiber_t* current_fiber;
    bool running;
    bool stopped;
    pthread_t thread;
    uint64_t tasks_executed;
    uint64_t steals_attempted;
    uint64_t steals_successful;
    int last_victim;
} worker_t;

typedef struct scheduler_config {
    size_t num_workers;
    size_t max_fibers;
    size_t stack_size;
    bool work_stealing;
    scheduler_backend_t backend;
    size_t io_uring_entries;
} scheduler_config_t;

typedef struct scheduler_stats {
    uint64_t total_fibers_created;
    uint64_t total_fibers_completed;
    uint64_t total_context_switches;
    uint64_t total_work_steals;
    uint64_t current_active_fibers;
    uint64_t current_ready_fibers;
    uint64_t total_io_submitted;
    uint64_t total_io_completed;
} scheduler_stats_t;

/* Batch scheduling support */
typedef struct spawn_batch {
    fiber_t** fibers;
    size_t count;
    size_t capacity;
} spawn_batch_t;

typedef struct scheduler {
    worker_t* workers;
    size_t num_workers;
    size_t next_worker;
    
    fiber_t* ready_queue;
    fiber_t* blocked_queue;
    
    scheduler_config_t config;
    scheduler_stats_t stats;
    
    bool running;
    bool initialized;
    
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    void* fiber_pool;
    
    scheduler_backend_t backend;
    
#ifdef __linux__
    io_uring_t io_uring_ring;
    bool io_uring_enabled;
    io_uring_submission_t *pending_submissions;
    pthread_mutex_t io_uring_mutex;
#endif
    
    fd_entry_t *fd_table;
    size_t fd_table_size;
    
    io_poller_t *pollers;
    pthread_mutex_t pollers_mutex;
    
    timer_node_t *timers;
    pthread_mutex_t timers_mutex;
    
    uint64_t current_time_ns;
} scheduler_t;

extern scheduler_t* g_scheduler;

int scheduler_init(scheduler_config_t* config);
void scheduler_shutdown(bool wait_for_completion);
scheduler_t* scheduler_get(void);

uint64_t scheduler_spawn(void (*entry)(void*), void* user_data);

/* Batch spawn for creating multiple fibers efficiently */
uint64_t* scheduler_spawn_batch(void (*entry)(void*), void** user_data_array, size_t count);

void scheduler_schedule(fiber_t* f, int worker_id);

/* Batch scheduling APIs */
spawn_batch_t* scheduler_create_spawn_batch(size_t initial_capacity);
void scheduler_destroy_spawn_batch(spawn_batch_t* batch);
int scheduler_spawn_batch_add(spawn_batch_t* batch, void (*entry)(void*), void* user_data);
void scheduler_spawn_batch_submit(spawn_batch_t* batch);

void scheduler_block(void* reason);
void scheduler_unblock(fiber_t* f);
void scheduler_yield(void);
void scheduler_wait(fiber_t* f);
void scheduler_wait_all(void);

void scheduler_get_stats(scheduler_stats_t* stats);
int scheduler_current_worker(void);
size_t scheduler_num_workers(void);

void scheduler_run(void);
void scheduler_stop(void);

void scheduler_set_backend(scheduler_backend_t backend);
scheduler_backend_t scheduler_get_backend(void);

int scheduler_submit_io(io_request_t *req);
int scheduler_wait_io(int fd, uint32_t events, int64_t timeout_ns);
void scheduler_wake_io(int fd, uint32_t events);

int scheduler_add_timer(uint64_t deadline_ns, fiber_t *fiber);
void scheduler_cancel_timer(fiber_t *fiber);

/* Native sleep - sleep for specified nanoseconds without asyncio */
void scheduler_sleep_ns(uint64_t ns);

int scheduler_register_fd(int fd, fiber_t *fiber, uint32_t events);
void scheduler_unregister_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif