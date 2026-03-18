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

void evloop_destroy(evloop_t *loop) {
    if (!loop) return;
    
    evloop_io_t *io = loop->io_list;
    while (io) {
        evloop_io_t *next = io->next;
        if (io->fd >= 0) {
            close(io->fd);
        }
        free(io);
        io = next;
    }
    
    evloop_timer_t *timer = loop->timer_list;
    while (timer) {
        evloop_timer_t *next = timer->next;
        if (timer->fd >= 0) {
            close(timer->fd);
        }
        free(timer);
        timer = next;
    }
    
    evloop_io_waiter_t *waiter = loop->waiter_list;
    while (waiter) {
        evloop_io_waiter_t *next = waiter->next;
        free(waiter);
        waiter = next;
    }
    
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
    }
    
#ifdef __linux__
    if (loop->io_uring_enabled) {
        io_uring_destroy(&loop->io_uring_ring);
    }
#endif
}

int evloop_add_io(evloop_t *loop, int fd, uint32_t events, evloop_io_callback_t callback, void *arg) {
    evloop_io_t *io = (evloop_io_t*)malloc(sizeof(evloop_io_t));
    if (!io) return -1;
    
    memset(io, 0, sizeof(*io));
    io->fd = fd;
    io->events = events;
    io->callback = callback;
    io->arg = arg;
    io->active = true;
    io->registered = true;
    io->user_data = (uint64_t)(uintptr_t)io;
    
    struct epoll_event ev;
    ev.events = events;
    ev.data.u64 = io->user_data;
    
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(io);
        return -1;
    }
    
    io->next = loop->io_list;
    loop->io_list = io;
    
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

void evloop_remove_timer(evloop_t *loop, evloop_timer_t *timer) {
    if (timer->fd >= 0) {
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, timer->fd, NULL);
        close(timer->fd);
        timer->fd = -1;
    }
    timer->active = false;
}

uint64_t evloop_now_ns(void) {
    return get_time_ns();
}

void evloop_update_time(evloop_t *loop) {
    loop->current_time_ns = get_time_ns();
}

void evloop_set_backend(evloop_t *loop, evloop_backend_t backend) {
#ifdef __linux__
    if (backend == EVLOOP_BACKEND_IOURING && !loop->io_uring_enabled) {
        if (io_uring_init(&loop->io_uring_ring, 256) == 0) {
            loop->io_uring_enabled = true;
            loop->backend = EVLOOP_BACKEND_IOURING;
        }
    } else if (backend == EVLOOP_BACKEND_EPOLL) {
        if (loop->io_uring_enabled) {
            io_uring_destroy(&loop->io_uring_ring);
            loop->io_uring_enabled = false;
        }
        loop->backend = EVLOOP_BACKEND_EPOLL;
    }
#else
    (void)loop;
    (void)backend;
#endif
}

evloop_backend_t evloop_get_backend(evloop_t *loop) {
    return loop->backend;
}

int evloop_register_fd(evloop_t *loop, int fd, uint64_t user_data) {
    evloop_io_t *io = (evloop_io_t*)malloc(sizeof(evloop_io_t));
    if (!io) return -1;
    
    memset(io, 0, sizeof(*io));
    io->fd = fd;
    io->events = EPOLLIN | EPOLLOUT;
    io->callback = NULL;
    io->arg = NULL;
    io->active = true;
    io->registered = true;
    io->user_data = user_data;
    
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.u64 = user_data;
    
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(io);
        return -1;
    }
    
    io->next = loop->io_list;
    loop->io_list = io;
    
    return 0;
}

int evloop_unregister_fd(evloop_t *loop, int fd) {
    evloop_remove_io(loop, fd);
    return 0;
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

int evloop_run(evloop_t *loop) {
    loop->running = true;
    
    const int MAX_EVENTS = 256;
    struct epoll_event events[MAX_EVENTS];
    
    while (loop->running) {
        evloop_update_time(loop);
        
        int timeout_ms = 100;
        evloop_timer_t *timer = loop->timer_list;
        while (timer && timer->active) {
            if (timer->next_fire_ns < loop->current_time_ns + (uint64_t)timeout_ms * 1000000) {
                timeout_ms = (int)((timer->next_fire_ns - loop->current_time_ns) / 1000000);
                if (timeout_ms < 0) timeout_ms = 0;
            }
            timer = timer->next;
        }
        
        if (loop->io_uring_enabled) {
            io_uring_submit(&loop->io_uring_ring);
            struct io_uring_cqe *cqe;
            int ret = io_uring_wait_cqe(&loop->io_uring_ring, &cqe);
            if (ret >= 0 && cqe) {
                process_io_uring_completions(loop);
            }
        }
        
        int nfds = epoll_wait(loop->epoll_fd, events, MAX_EVENTS, timeout_ms);
        
        for (int i = 0; i < nfds; i++) {
            uint64_t user_data = events[i].data.u64;
            
            evloop_timer_t *timer = (evloop_timer_t*)events[i].data.ptr;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                continue;
            }
            
            if (events[i].events & EPOLLIN) {
                evloop_io_t *io = loop->io_list;
                while (io) {
                    if (io->fd == events[i].data.fd && io->active && io->callback) {
                        io->callback(loop, io->fd, events[i].events, io->arg);
                        break;
                    }
                    io = io->next;
                }
                
                timer = loop->timer_list;
                while (timer) {
                    if (timer->fd == events[i].data.fd && timer->active && timer->callback) {
                        timer->callback(timer, timer->arg);
                        break;
                    }
                    timer = timer->next;
                }
            }
        }
        
        evloop_timer_t **tprev = &loop->timer_list;
        timer = loop->timer_list;
        while (timer && timer->active) {
            if (timer->next_fire_ns <= loop->current_time_ns) {
                if (timer->callback) {
                    timer->callback(timer, timer->arg);
                }
                
                if (timer->type == EVLOOP_TIMER_PERIODIC) {
                    timer->next_fire_ns = loop->current_time_ns + timer->interval_ns;
                    tprev = &timer->next;
                    timer = timer->next;
                } else {
                    *tprev = timer->next;
                    timer->active = false;
                    timer = *tprev;
                }
            } else {
                tprev = &timer->next;
                timer = timer->next;
            }
        }
    }
    
    return 0;
}

void evloop_stop(evloop_t *loop) {
    loop->running = false;
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

int evloop_wait_io(evloop_t *loop, int fd, uint32_t events) {
    fiber_t *current = fiber_current();
    if (!current) {
        return -1;
    }
    
    evloop_io_waiter_t *waiter = (evloop_io_waiter_t*)malloc(sizeof(evloop_io_waiter_t));
    if (!waiter) return -1;
    
    waiter->fiber = current;
    waiter->fd = fd;
    waiter->events = events;
    waiter->active = true;
    waiter->next = loop->waiter_list;
    loop->waiter_list = waiter;
    
    current->state = FIBER_WAITING;
    current->waiting_on = waiter;
    
    struct epoll_event ev;
    ev.events = (events & EVLOOP_READ) ? EPOLLIN : 0;
    ev.events |= (events & EVLOOP_WRITE) ? EPOLLOUT : 0;
    ev.events |= EPOLLET;
    ev.data.u64 = (uint64_t)(uintptr_t)waiter;
    
    epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    
    fiber_yield();
    
    return 0;
}

void evloop_wake_io(evloop_t *loop, int fd, uint32_t events) {
    evloop_io_waiter_t **prev = &loop->waiter_list;
    evloop_io_waiter_t *waiter = loop->waiter_list;
    
    while (waiter) {
        if (waiter->fd == fd && waiter->active) {
            if ((events & EPOLLIN && (waiter->events & EVLOOP_READ)) ||
                (events & EPOLLOUT && (waiter->events & EVLOOP_WRITE))) {
                *prev = waiter->next;
                
                epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                
                waiter->active = false;
                
                if (waiter->fiber) {
                    waiter->fiber->state = FIBER_READY;
                    waiter->fiber->waiting_on = NULL;
                    scheduler_schedule(waiter->fiber, -1);
                }
                
                free(waiter);
                return;
            }
        }
        prev = &waiter->next;
        waiter = waiter->next;
    }
}