#ifndef EVLOOP_H
#define EVLOOP_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

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

typedef struct evloop_timer_s evloop_timer_t;
typedef struct evloop_io_s evloop_io_t;
typedef struct evloop_s evloop_t;

typedef void (*evloop_callback_t)(void *arg);
typedef void (*evloop_io_callback_t)(evloop_t *loop, int fd, uint32_t events, void *arg);
typedef void (*evloop_timer_callback_t)(evloop_timer_t *timer, void *arg);

struct evloop_timer_s {
    evloop_timer_t *next;
    uint64_t interval_ns;
    uint64_t next_fire_ns;
    evloop_timer_type_t type;
    evloop_timer_callback_t callback;
    void *arg;
    bool active;
};

struct evloop_io_s {
    evloop_io_t *next;
    int fd;
    uint32_t events;
    evloop_io_callback_t callback;
    void *arg;
    bool active;
};

struct evloop_s {
    int epoll_fd;
    bool running;
    
    evloop_io_t *io_list;
    evloop_timer_t *timer_list;
    
    uint64_t current_time_ns;
    
    int worker_count;
    int current_worker;
    
    void *user_data;
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

#endif
