#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fiber fiber_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOCKET_STATE_CLOSED = 0,
    SOCKET_STATE_LISTENING = 1,
    SOCKET_STATE_CONNECTING = 2,
    SOCKET_STATE_CONNECTED = 3,
    SOCKET_STATE_CLOSING = 4
} socket_state_t;

typedef struct gsocket gsocket_t;
typedef struct socket_io socket_io_t;

typedef void (*socket_callback_t)(gsocket_t *sock, void *arg);
typedef void (*socket_io_callback_t)(gsocket_t *sock, ssize_t result, void *arg);

struct gsocket {
    int fd;
    socket_state_t state;
    bool nonblocking;
    bool reuseaddr;
    bool reuseport;
    bool nodelay;
    
    socket_callback_t on_read;
    socket_callback_t on_write;
    socket_callback_t on_connect;
    socket_callback_t on_accept;
    socket_callback_t on_close;
    void *callback_arg;
    
    void *user_data;
    
    gsocket_t *next;
};

typedef struct async_connect_req {
    gsocket_t *sock;
    const char *host;
    uint16_t port;
    fiber_t *fiber;
    bool completed;
    int result;
} async_connect_req_t;

typedef struct async_accept_req {
    gsocket_t *server_sock;
    gsocket_t *client_sock;
    fiber_t *fiber;
    bool completed;
} async_accept_req_t;

typedef struct async_read_req {
    gsocket_t *sock;
    void *buf;
    size_t len;
    fiber_t *fiber;
    ssize_t result;
    bool completed;
} async_read_req_t;

typedef struct async_write_req {
    gsocket_t *sock;
    const void *buf;
    size_t len;
    fiber_t *fiber;
    ssize_t result;
    bool completed;
} async_write_req_t;

int net_init(void);
void net_shutdown(void);

gsocket_t* gsocket_create(void);
void gsocket_destroy(gsocket_t *sock);

int gsocket_set_nonblocking(gsocket_t *sock, bool nonblocking);
int gsocket_set_reuseaddr(gsocket_t *sock, bool reuse);
int gsocket_set_reuseport(gsocket_t *sock, bool reuse);
int gsocket_set_nodelay(gsocket_t *sock, bool nodelay);

int gsocket_bind(gsocket_t *sock, const char *host, uint16_t port);
int gsocket_listen(gsocket_t *sock, int backlog);
int gsocket_connect(gsocket_t *sock, const char *host, uint16_t port);
gsocket_t* gsocket_accept(gsocket_t *server_sock);

ssize_t gsocket_read(gsocket_t *sock, void *buf, size_t len);
ssize_t gsocket_write(gsocket_t *sock, const void *buf, size_t len);
ssize_t gsocket_recv(gsocket_t *sock, void *buf, size_t len);
ssize_t gsocket_send(gsocket_t *sock, const void *buf, size_t len);

int gsocket_close(gsocket_t *sock);

int gsocket_getpeername(gsocket_t *sock, struct sockaddr *addr, socklen_t *addrlen);
int gsocket_getsockname(gsocket_t *sock, struct sockaddr *addr, socklen_t *addrlen);

int gsocket_async_connect(gsocket_t *sock, const char *host, uint16_t port);
gsocket_t* gsocket_async_accept(gsocket_t *server_sock);
ssize_t gsocket_async_read(gsocket_t *sock, void *buf, size_t len);
ssize_t gsocket_async_write(gsocket_t *sock, const void *buf, size_t len);
ssize_t gsocket_async_recv(gsocket_t *sock, void *buf, size_t len);
ssize_t gsocket_async_send(gsocket_t *sock, const void *buf, size_t len);

int gsocket_wait_readable(gsocket_t *sock, int64_t timeout_ns);
int gsocket_wait_writable(gsocket_t *sock, int64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif