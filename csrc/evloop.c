#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include "evloop.h"
#include "scheduler.h"

#ifdef __linux__
#include "io_uring.h"
#endif

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int evloop_init(evloop_t *loop, int worker_count) {
    memset(loop, 0, sizeof(*loop));
    
    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        return -1;
    }
    
    loop->worker_count = worker_count;
    loop->current_worker = 0;
    loop->running = false;
    loop->current_time_ns = get_time_ns();
    loop->backend = EVLOOP_BACKEND_EPOLL;
    
#ifdef __linux__
    loop->io_uring_enabled = false;
#endif
    
    return 0;
}

void evloop_remove_io(evloop_t *loop, int fd) {
    evloop_io_t **prev = &loop->io_list;
    evloop_io_t *io = loop->io_list;
    
    while (io) {
        if (io->fd == fd) {
            *prev = io->next;
            epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            io->active = false;
            return;
        }
        prev = &io->next;
        io = io->next;
    }
}

int evloop_add_timer(evloop_t *loop, evloop_timer_t *timer, uint64_t interval_ns,
                    evloop_timer_type_t type, evloop_timer_callback_t callback, void *arg) {
    memset(timer, 0, sizeof(*timer));
    timer->interval_ns = interval_ns;
    timer->type = type;
    timer->callback = callback;
    timer->arg = arg;
    timer->active = true;
    timer->next_fire_ns = loop->current_time_ns + interval_ns;
    
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) return -1;
    
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = interval_ns / 1000;
    if (type == EVLOOP_TIMER_PERIODIC) {
        its.it_interval.tv_nsec = interval_ns / 1000;
    }
    timerfd_settime(tfd, 0, &its, NULL);
    
    timer->fd = tfd;
    timer->next = loop->timer_list;
    loop->timer_list = timer;
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = timer;
    epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, tfd, &ev);
    
    return tfd;
}

uint64_t evloop_now_ns(void) {
    return get_time_ns();
}

void evloop_update_time(evloop_t *loop) {
    loop->current_time_ns = get_time_ns();
}

static void process_io_uring_completions(evloop_t *loop) {
#ifdef __linux__
    if (!loop->io_uring_enabled) return;
    
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&loop->io_uring_ring, &cqe) == 1) {
        uint64_t user_data = cqe->user_data;
        int res = cqe->res;
        
        io_uring_cqe_seen(&loop->io_uring_ring, cqe);
        
        evloop_io_t *io = loop->io_list;
        while (io) {
            if (io->user_data == user_data && io->callback) {
                uint32_t events = 0;
                if (res >= 0) {
                    events = EPOLLIN | EPOLLOUT;
                } else {
                    events = EPOLLERR;
                }
                io->callback(loop, io->fd, events, io->arg);
                break;
            }
            io = io->next;
        }
    }
#endif
}

int evloop_read_async(evloop_t *loop, int fd, void *buf, size_t len, uint64_t offset,
                      evloop_io_completion_t completion, void *arg) {
#ifdef __linux__
    if (loop->io_uring_enabled) {
        uint64_t user_data = (uint64_t)(uintptr_t)arg;
        return io_uring_read(&loop->io_uring_ring, fd, buf, len, offset, user_data);
    }
#endif
    (void)loop;
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    (void)completion;
    (void)arg;
    return -1;
}

int evloop_write_async(evloop_t *loop, int fd, const void *buf, size_t len, uint64_t offset,
                       evloop_io_completion_t completion, void *arg) {
#ifdef __linux__
    if (loop->io_uring_enabled) {
        uint64_t user_data = (uint64_t)(uintptr_t)arg;
        return io_uring_write(&loop->io_uring_ring, fd, buf, len, offset, user_data);
    }
#endif
    (void)loop;
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    (void)completion;
    (void)arg;
    return -1;
}

int evloop_accept_async(evloop_t *loop, int fd, struct sockaddr *addr, socklen_t *addrlen,
                        evloop_io_completion_t completion, void *arg) {
#ifdef __linux__
    if (loop->io_uring_enabled) {
        uint64_t user_data = (uint64_t)(uintptr_t)arg;
        return io_uring_accept(&loop->io_uring_ring, fd, addr, addrlen, user_data);
    }
#endif
    (void)loop;
    (void)fd;
    (void)addr;
    (void)addrlen;
    (void)completion;
    (void)arg;
    return -1;
}

int evloop_connect_async(evloop_t *loop, int fd, const struct sockaddr *addr, socklen_t addrlen,
                         evloop_io_completion_t completion, void *arg) {
#ifdef __linux__
    if (loop->io_uring_enabled) {
        uint64_t user_data = (uint64_t)(uintptr_t)arg;
        return io_uring_connect(&loop->io_uring_ring, fd, addr, addrlen, user_data);
    }
#endif
    (void)loop;
    (void)fd;
    (void)addr;
    (void)addrlen;
    (void)completion;
    (void)arg;
    return -1;
}

