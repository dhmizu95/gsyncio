/**
 * native_io.c - Native I/O operations for gsyncio
 * 
 * Provides epoll-based async I/O without asyncio dependency.
 */

#include "native_io.h"
#include "scheduler.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ============================================ */
/* Global State                                 */
/* ============================================ */

static native_evloop_t g_evloop;
static bool g_io_initialized = false;

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

int native_io_init(void) {
    if (g_io_initialized) {
        return 0;  /* Already initialized */
    }

    memset(&g_evloop, 0, sizeof(g_evloop));
    
    g_evloop.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_evloop.epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /* Allocate events array */
    g_evloop.events = (struct epoll_event*)calloc(NATIVE_IO_MAX_EVENTS, sizeof(struct epoll_event));
    if (!g_evloop.events) {
        close(g_evloop.epoll_fd);
        return -1;
    }

    g_evloop.running = true;
    g_evloop.event_count = 0;
    
    g_io_initialized = true;
    return 0;
}

void native_io_shutdown(void) {
    if (!g_io_initialized) {
        return;
    }

    g_evloop.running = false;
    
    if (g_evloop.epoll_fd >= 0) {
        close(g_evloop.epoll_fd);
        g_evloop.epoll_fd = -1;
    }

    if (g_evloop.events) {
        free(g_evloop.events);
        g_evloop.events = NULL;
    }

    g_io_initialized = false;
}

native_evloop_t* native_io_get_evloop(void) {
    return &g_evloop;
}

/* ============================================ */
/* Socket Operations                            */
/* ============================================ */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int native_socket_tcp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    /* Set non-blocking */
    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    /* Set SO_REUSEADDR */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    return fd;
}

int native_socket_udp(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    /* Set non-blocking */
    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int native_socket_bind(int fd, const char* host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host == NULL || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            /* Try hostname resolution */
            struct hostent* he = gethostbyname(host);
            if (!he) {
                return -1;
            }
            memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        }
    }

    return bind(fd, (struct sockaddr*)&addr, sizeof(addr));
}

int native_socket_listen(int fd, int backlog) {
    return listen(fd, backlog);
}

int native_socket_accept(int fd, struct sockaddr_in* client_addr) {
    fiber_t* current = fiber_current();
    if (!current) {
        /* Not in fiber context - use blocking accept */
        socklen_t addr_len = sizeof(struct sockaddr_in);
        return accept(fd, (struct sockaddr*)client_addr, &addr_len);
    }

    /* Fiber-aware accept - wait for readability */
    while (1) {
        socklen_t addr_len = sizeof(struct sockaddr_in);
        int client_fd = accept(fd, (struct sockaddr*)client_addr, &addr_len);
        
        if (client_fd >= 0) {
            /* Set non-blocking on client socket */
            set_nonblocking(client_fd);
            return client_fd;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Wait for readability */
            scheduler_wait_io(fd, EPOLLIN, -1);
        } else {
            return -1;  /* Error */
        }
    }
}

int native_socket_connect(int fd, const char* host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host);
        if (!he) {
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    fiber_t* current = fiber_current();
    if (!current) {
        /* Not in fiber context - use blocking connect */
        return connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    }

    /* Fiber-aware connect */
    int result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    
    if (result < 0 && (errno == EINPROGRESS || errno == EAGAIN)) {
        /* Wait for writability (connection completed) */
        scheduler_wait_io(fd, EPOLLOUT, 5000000000LL);  /* 5 second timeout */
        
        /* Check if connection succeeded */
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
            return -1;
        }
        return error == 0 ? 0 : -1;
    }

    return result;
}

ssize_t native_socket_send(int fd, const void* buffer, size_t len) {
    fiber_t* current = fiber_current();
    
    while (1) {
        ssize_t sent = send(fd, buffer, len, 0);
        
        if (sent >= 0) {
            return sent;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (current) {
                /* Wait for writability */
                scheduler_wait_io(fd, EPOLLOUT, -1);
            } else {
                return -1;  /* Not in fiber context */
            }
        } else {
            return -1;  /* Error */
        }
    }
}

ssize_t native_socket_recv(int fd, void* buffer, size_t len) {
    fiber_t* current = fiber_current();
    
    while (1) {
        ssize_t received = recv(fd, buffer, len, 0);
        
        if (received >= 0) {
            return received;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (current) {
                /* Wait for readability */
                scheduler_wait_io(fd, EPOLLIN, -1);
            } else {
                return -1;  /* Not in fiber context */
            }
        } else {
            return -1;  /* Error */
        }
    }
}

void native_socket_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

/* ============================================ */
/* Async I/O (Fiber-aware)                      */
/* ============================================ */

ssize_t native_io_read_async(int fd, void* buffer, size_t len) {
    return native_socket_recv(fd, buffer, len);
}

ssize_t native_io_write_async(int fd, const void* buffer, size_t len) {
    return native_socket_send(fd, buffer, len);
}

/* ============================================ */
/* Event Loop                                   */
/* ============================================ */

int native_evloop_run_once(native_evloop_t* evloop) {
    if (!evloop || !evloop->running) {
        return 0;
    }

    int nfds = epoll_wait(evloop->epoll_fd, evloop->events, 
                          NATIVE_IO_MAX_EVENTS, NATIVE_IO_EPOLL_TIMEOUT_MS);
    
    if (nfds < 0) {
        if (errno == EINTR) {
            return 0;  /* Interrupted, retry */
        }
        return -1;
    }

    evloop->event_count = nfds;
    
    /* Wake up fibers waiting on these FDs */
    for (int i = 0; i < nfds; i++) {
        uint32_t events = evloop->events[i].events;
        int fd = evloop->events[i].data.fd;
        
        /* Wake up waiting fibers */
        scheduler_wake_io(fd, events);
    }

    return nfds;
}

int native_evloop_register(native_evloop_t* evloop, int fd, uint32_t events, void* user_data) {
    if (!evloop || fd < 0) {
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events | EPOLLET;  /* Edge-triggered */
    ev.data.fd = fd;
    ev.data.ptr = user_data;

    return epoll_ctl(evloop->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

void native_evloop_unregister(native_evloop_t* evloop, int fd) {
    if (!evloop || fd < 0) {
        return;
    }

    epoll_ctl(evloop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}
