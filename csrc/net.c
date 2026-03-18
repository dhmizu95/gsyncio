#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include "net.h"
#include "evloop.h"
#include "scheduler.h"

static int g_net_initialized = 0;
static evloop_t *g_evloop = NULL;

int net_init(void) {
    if (g_net_initialized) {
        return 0;
    }
    g_net_initialized = 1;
    return 0;
}

void net_shutdown(void) {
    g_net_initialized = 0;
}

void net_set_evloop(evloop_t *loop) {
    g_evloop = loop;
}

static int set_nonblocking(int fd, bool nonblocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    
    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    
    return fcntl(fd, F_SETFL, flags);
}

static int set_reuseaddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

static int set_reuseport(int fd) {
#ifdef SO_REUSEPORT
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#else
    (void)fd;
    return 0;
#endif
}

static int set_nodelay(int fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

gsocket_t* gsocket_create(void) {
    gsocket_t *sock = (gsocket_t*)calloc(1, sizeof(gsocket_t));
    if (!sock) {
        return NULL;
    }
    
    sock->fd = -1;
    sock->state = SOCKET_STATE_CLOSED;
    sock->nonblocking = true;
    
    return sock;
}

void gsocket_destroy(gsocket_t *sock) {
    if (!sock) {
        return;
    }
    
    if (sock->fd >= 0) {
        close(sock->fd);
    }
    
    free(sock);
}

int gsocket_set_nonblocking(gsocket_t *sock, bool nonblocking) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    sock->nonblocking = nonblocking;
    return set_nonblocking(sock->fd, nonblocking);
}

int gsocket_set_reuseaddr(gsocket_t *sock, bool reuse) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    sock->reuseaddr = reuse;
    return set_reuseaddr(sock->fd);
}

int gsocket_set_reuseport(gsocket_t *sock, bool reuse) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    sock->reuseport = reuse;
    return set_reuseport(sock->fd);
}

int gsocket_set_nodelay(gsocket_t *sock, bool nodelay) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    sock->nodelay = nodelay;
    return set_nodelay(sock->fd);
}

int gsocket_bind(gsocket_t *sock, const char *host, uint16_t port) {
    if (!sock) {
        return -1;
    }
    
    sock->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock->fd < 0) {
        return -1;
    }
    
    if (sock->reuseaddr) {
        set_reuseaddr(sock->fd);
    }
    if (sock->reuseport) {
        set_reuseport(sock->fd);
    }
    
    set_nonblocking(sock->fd, sock->nonblocking);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host && strcmp(host, "0.0.0.0") != 0 && strcmp(host, "*") != 0) {
        inet_pton(AF_INET, host, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock->fd);
        sock->fd = -1;
        return -1;
    }
    
    sock->state = SOCKET_STATE_LISTENING;
    return 0;
}

int gsocket_listen(gsocket_t *sock, int backlog) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    if (listen(sock->fd, backlog) < 0) {
        return -1;
    }
    
    sock->state = SOCKET_STATE_LISTENING;
    return 0;
}

int gsocket_connect(gsocket_t *sock, const char *host, uint16_t port) {
    if (!sock) {
        return -1;
    }
    
    sock->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock->fd < 0) {
        return -1;
    }
    
    set_nonblocking(sock->fd, sock->nonblocking);
    if (sock->nodelay) {
        set_nodelay(sock->fd);
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            close(sock->fd);
            sock->fd = -1;
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    sock->state = SOCKET_STATE_CONNECTING;
    
    int ret = connect(sock->fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(sock->fd);
        sock->fd = -1;
        sock->state = SOCKET_STATE_CLOSED;
        return -1;
    }
    
    return 0;
}

gsocket_t* gsocket_accept(gsocket_t *server_sock) {
    if (!server_sock || server_sock->fd < 0) {
        return NULL;
    }
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_fd = accept(server_sock->fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return NULL;
        }
        return NULL;
    }
    
    set_nonblocking(client_fd, server_sock->nonblocking);
    set_nodelay(client_fd);
    
    gsocket_t *client_sock = gsocket_create();
    if (!client_sock) {
        close(client_fd);
        return NULL;
    }
    
    client_sock->fd = client_fd;
    client_sock->state = SOCKET_STATE_CONNECTED;
    client_sock->nonblocking = server_sock->nonblocking;
    client_sock->nodelay = true;
    
    return client_sock;
}

ssize_t gsocket_read(gsocket_t *sock, void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    ssize_t n = read(sock->fd, buf, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -2;
    }
    
    return n;
}

ssize_t gsocket_write(gsocket_t *sock, const void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    ssize_t n = write(sock->fd, buf, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -2;
    }
    
    return n;
}

ssize_t gsocket_recv(gsocket_t *sock, void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    ssize_t n = recv(sock->fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -2;
    }
    
    return n;
}

ssize_t gsocket_send(gsocket_t *sock, const void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    ssize_t n = send(sock->fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -2;
    }
    
    return n;
}

int gsocket_close(gsocket_t *sock) {
    if (!sock) {
        return -1;
    }
    
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
    
    sock->state = SOCKET_STATE_CLOSED;
    return 0;
}

int gsocket_getpeername(gsocket_t *sock, struct sockaddr *addr, socklen_t *addrlen) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    return getpeername(sock->fd, addr, addrlen);
}

int gsocket_getsockname(gsocket_t *sock, struct sockaddr *addr, socklen_t *addrlen) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    return getsockname(sock->fd, addr, addrlen);
}

int gsocket_async_connect(gsocket_t *sock, const char *host, uint16_t port) {
    if (!sock) {
        return -1;
    }
    
    int ret = gsocket_connect(sock, host, port);
    if (ret < 0) {
        return ret;
    }
    
    if (sock->state == SOCKET_STATE_CONNECTING) {
        if (gsocket_wait_writable(sock, -1) < 0) {
            return -1;
        }
        
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            sock->state = SOCKET_STATE_CLOSED;
            return -1;
        }
    }
    
    sock->state = SOCKET_STATE_CONNECTED;
    return 0;
}

gsocket_t* gsocket_async_accept(gsocket_t *server_sock) {
    if (!server_sock || server_sock->fd < 0) {
        return NULL;
    }
    
    while (1) {
        gsocket_t *client = gsocket_accept(server_sock);
        if (client) {
            return client;
        }
        
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return NULL;
        }
        
        if (gsocket_wait_readable(server_sock, -1) < 0) {
            return NULL;
        }
    }
}

ssize_t gsocket_async_read(gsocket_t *sock, void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    while (1) {
        ssize_t n = gsocket_read(sock, buf, len);
        if (n != -2) {
            return n;
        }
        
        if (gsocket_wait_readable(sock, -1) < 0) {
            return -1;
        }
    }
}

ssize_t gsocket_async_write(gsocket_t *sock, const void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    while (1) {
        ssize_t n = gsocket_write(sock, buf, len);
        if (n != -2) {
            return n;
        }
        
        if (gsocket_wait_writable(sock, -1) < 0) {
            return -1;
        }
    }
}

ssize_t gsocket_async_recv(gsocket_t *sock, void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    while (1) {
        ssize_t n = gsocket_recv(sock, buf, len);
        if (n != -2) {
            return n;
        }
        
        if (gsocket_wait_readable(sock, -1) < 0) {
            return -1;
        }
    }
}

ssize_t gsocket_async_send(gsocket_t *sock, const void *buf, size_t len) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    
    while (1) {
        ssize_t n = gsocket_send(sock, buf, len);
        if (n != -2) {
            return n;
        }
        
        if (gsocket_wait_writable(sock, -1) < 0) {
            return -1;
        }
    }
}

static int wait_fd(int fd, uint32_t events, int64_t timeout_ns) {
    static evloop_t default_loop;
    static int default_loop_init = 0;
    
    evloop_t *loop = g_evloop;
    
    if (!loop && !default_loop_init) {
        evloop_init(&default_loop, 1);
        default_loop_init = 1;
        loop = &default_loop;
    } else if (!loop) {
        loop = &default_loop;
    }
    
    if (timeout_ns < 0) {
        timeout_ns = 10000000000LL;
    }
    
    uint64_t start = evloop_now_ns();
    uint64_t deadline = start + timeout_ns;
    
    while (1) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10000000;
        
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (events & EVLOOP_READ) {
            FD_SET(fd, &read_fds);
        }
        if (events & EVLOOP_WRITE) {
            FD_SET(fd, &write_fds);
        }
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        
        int ret = select(fd + 1, &read_fds, &write_fds, NULL, &tv);
        
        if (ret > 0) {
            return 0;
        }
        
        if (ret < 0 && errno != EINTR) {
            return -1;
        }
        
        if (evloop_now_ns() >= deadline) {
            errno = EAGAIN;
            return -1;
        }
        
        fiber_yield();
    }
}

int gsocket_wait_readable(gsocket_t *sock, int64_t timeout_ns) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    return wait_fd(sock->fd, EVLOOP_READ, timeout_ns);
}

int gsocket_wait_writable(gsocket_t *sock, int64_t timeout_ns) {
    if (!sock || sock->fd < 0) {
        return -1;
    }
    return wait_fd(sock->fd, EVLOOP_WRITE, timeout_ns);
}