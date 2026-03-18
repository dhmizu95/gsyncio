#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

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

typedef void (*socket_callback_t)(gsocket_t *sock, void *arg);

struct gsocket {
    int fd;
    socket_state_t state;
    bool is_server;
    
    socket_callback_t on_read;
    socket_callback_t on_write;
    socket_callback_t on_connect;
    socket_callback_t on_accept;
    socket_callback_t on_close;
    void *callback_arg;
    
    void *user_data;
    
    gsocket_t *next;
};

int net_init(void);
void net_shutdown(void);

gsocket_t* gsocket_create(void);
void gsocket_destroy(gsocket_t *sock);

int gsocket_set_nonblocking(gsocket_t *sock, bool nonblocking);
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

#ifdef __cplusplus
}
#endif

#endif
