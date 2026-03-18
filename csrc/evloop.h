#ifndef EVLOOP_H
#define EVLOOP_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __linux__
#include "io_uring.h"
#endif

typedef enum {
    EVLOOP_NONE = 0,
    EVLOOP_READ = 1,
    EVLOOP_WRITE = 2,
    EVLOOP_READ_WRITE = 3
} evloop_event_t;

typedef enum {
    EVLOOP_TIMER_ONCE = 0,
    EVLOOP_TIMER_PERIODIC = 1
} evloop_timer_type_t;

typedef enum {
    EVLOOP_BACKEND_EPOLL = 0,
    EVLOOP_BACKEND_IOURING = 1
} evloop_backend_t;

typedef struct fiber fiber_t;

typedef struct evloop_timer_s evloop_timer_t;
typedef struct evloop_io_s evloop_io_t;
typedef struct evloop_s evloop_t;
typedef struct evloop_pending_io evloop_pending_io_t;

typedef void (*evloop_callback_t)(void *arg);
typedef void (*evloop_io_callback_t)(evloop_t *loop, int fd, uint32_t events, void *arg);
typedef void (*evloop_timer_callback_t)(evloop_timer_t *timer, void *arg);
typedef void (*evloop_io_completion_t)(void *arg, int64_t result);

struct evloop_timer_s {
    evloop_timer_t *next;
    uint64_t interval_ns;
    uint64_t next_fire_ns;
    evloop_timer_type_t type;
    evloop_timer_callback_t callback;
    void *arg;
    bool active;
    int fd;
};

struct evloop_io_s {
    evloop_io_t *next;
    int fd;
    uint32_t events;
    evloop_io_callback_t callback;
    void *arg;
    bool active;
    bool registered;
    uint64_t user_data;
};

struct evloop_pending_io {
    struct evloop_pending_io *next;
    int fd;
    uint32_t op;
    void *buf;
    size_t len;
    int64_t offset;
    uint64_t user_data;
    evloop_io_completion_t completion;
    void *completion_arg;
    fiber_t *fiber;
};

typedef struct evloop_pending_io evloop_pending_io_t;

typedef struct evloop_io_waiter {
    fiber_t *fiber;
    int fd;
    uint32_t events;
    bool active;
    struct evloop_io_waiter *next;
} evloop_io_waiter_t;

struct evloop_s {
    int epoll_fd;
    bool running;
    evloop_backend_t backend;

    evloop_io_t *io_list;
    evloop_timer_t *timer_list;
    evloop_io_waiter_t *waiter_list;

    uint64_t current_time_ns;

    int worker_count;
    int current_worker;

    void *user_data;

#ifdef __linux__
    io_uring_t io_uring_ring;
    bool io_uring_enabled;
#endif
};

int evloop_init(evloop_t *loop, int worker_count);
void evloop_destroy(evloop_t *loop);

int evloop_add_io(evloop_t *loop, int fd, uint32_t events, evloop_io_callback_t callback, void *arg);
void evloop_remove_io(evloop_t *loop, int fd);

int evloop_add_timer(evloop_t *loop, evloop_timer_t *timer, uint64_t interval_ns, 
                    evloop_timer_type_t type, evloop_timer_callback_t callback, void *arg);
void evloop_remove_timer(evloop_t *loop, evloop_timer_t *timer);

int evloop_run(evloop_t *loop);
void evloop_stop(evloop_t *loop);

uint64_t evloop_now_ns(void);
void evloop_update_time(evloop_t *loop);

int evloop_register_fd(evloop_t *loop, int fd, uint64_t user_data);
int evloop_unregister_fd(evloop_t *loop, int fd);

int evloop_read_async(evloop_t *loop, int fd, void *buf, size_t len, uint64_t offset,
                      evloop_io_completion_t completion, void *arg);
int evloop_write_async(evloop_t *loop, int fd, const void *buf, size_t len, uint64_t offset,
                       evloop_io_completion_t completion, void *arg);
int evloop_accept_async(evloop_t *loop, int fd, struct sockaddr *addr, socklen_t *addrlen,
                        evloop_io_completion_t completion, void *arg);
int evloop_connect_async(evloop_t *loop, int fd, const struct sockaddr *addr, socklen_t addrlen,
                         evloop_io_completion_t completion, void *arg);

int evloop_wait_io(evloop_t *loop, int fd, uint32_t events);
void evloop_wake_io(evloop_t *loop, int fd, uint32_t events);

void evloop_set_backend(evloop_t *loop, evloop_backend_t backend);
evloop_backend_t evloop_get_backend(evloop_t *loop);

#endif