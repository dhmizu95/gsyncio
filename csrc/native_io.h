/**
 * native_io.h - Native I/O operations for gsyncio
 * 
 * Provides epoll-based async I/O without asyncio dependency.
 */

#ifndef NATIVE_IO_H
#define NATIVE_IO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================ */
/* Configuration                                */
/* ============================================ */

#define NATIVE_IO_MAX_EVENTS 1024
#define NATIVE_IO_EPOLL_TIMEOUT_MS 100

/* ============================================ */
/* I/O Operations                               */
/* ============================================ */

typedef enum {
    NATIVE_IO_READ = 1,
    NATIVE_IO_WRITE = 2,
    NATIVE_IO_ACCEPT = 3,
    NATIVE_IO_CONNECT = 4,
    NATIVE_IO_CLOSE = 5
} native_io_op_t;

/* ============================================ */
/* Socket Types                                 */
/* ============================================ */

typedef struct {
    int fd;
    bool is_server;
    bool is_closed;
    struct sockaddr_in addr;
} native_socket_t;

/* ============================================ */
/* Event Loop                                   */
/* ============================================ */

#include <sys/epoll.h>

typedef struct {
    int epoll_fd;
    bool running;
    int event_count;
    struct epoll_event* events;  /* Dynamically allocated */
} native_evloop_t;

/* ============================================ */
/* I/O Request                                  */
/* ============================================ */

typedef struct native_io_request {
    int fd;
    native_io_op_t op;
    void* buffer;
    size_t len;
    size_t offset;
    void* user_data;
    void (*callback)(struct native_io_request*, ssize_t result);
    struct native_io_request* next;
} native_io_request_t;

/* ============================================ */
/* Lifecycle                                    */
/* ============================================ */

/**
 * Initialize native I/O subsystem
 * @return 0 on success, -1 on failure
 */
int native_io_init(void);

/**
 * Shutdown native I/O subsystem
 */
void native_io_shutdown(void);

/**
 * Get the event loop instance
 * @return Event loop pointer
 */
native_evloop_t* native_io_get_evloop(void);

/* ============================================ */
/* Socket Operations                            */
/* ============================================ */

/**
 * Create a new TCP socket
 * @return Socket FD, or -1 on error
 */
int native_socket_tcp(void);

/**
 * Create a new UDP socket
 * @return Socket FD, or -1 on error
 */
int native_socket_udp(void);

/**
 * Bind socket to address
 * @param fd Socket FD
 * @param host Host address (e.g., "0.0.0.0")
 * @param port Port number
 * @return 0 on success, -1 on error
 */
int native_socket_bind(int fd, const char* host, int port);

/**
 * Listen for connections (TCP server)
 * @param fd Socket FD
 * @param backlog Maximum pending connections
 * @return 0 on success, -1 on error
 */
int native_socket_listen(int fd, int backlog);

/**
 * Accept a connection (non-blocking, fiber-aware)
 * @param fd Server socket FD
 * @param client_addr Optional: output client address
 * @return Client socket FD, or -1 on error
 */
int native_socket_accept(int fd, struct sockaddr_in* client_addr);

/**
 * Connect to remote host (non-blocking, fiber-aware)
 * @param fd Socket FD
 * @param host Remote host
 * @param port Remote port
 * @return 0 on success, -1 on error
 */
int native_socket_connect(int fd, const char* host, int port);

/**
 * Send data (non-blocking, fiber-aware)
 * @param fd Socket FD
 * @param buffer Data buffer
 * @param len Data length
 * @return Bytes sent, or -1 on error
 */
ssize_t native_socket_send(int fd, const void* buffer, size_t len);

/**
 * Receive data (non-blocking, fiber-aware)
 * @param fd Socket FD
 * @param buffer Output buffer
 * @param len Buffer size
 * @return Bytes received, or -1 on error
 */
ssize_t native_socket_recv(int fd, void* buffer, size_t len);

/**
 * Close socket
 * @param fd Socket FD
 */
void native_socket_close(int fd);

/* ============================================ */
/* Async I/O (Fiber-aware)                      */
/* ============================================ */

/**
 * Async read - blocks current fiber until data available
 * @param fd File descriptor
 * @param buffer Output buffer
 * @param len Buffer size
 * @return Bytes read, or -1 on error
 */
ssize_t native_io_read_async(int fd, void* buffer, size_t len);

/**
 * Async write - blocks current fiber until data written
 * @param fd File descriptor
 * @param buffer Data buffer
 * @param len Data length
 * @return Bytes written, or -1 on error
 */
ssize_t native_io_write_async(int fd, const void* buffer, size_t len);

/**
 * Run event loop iteration
 * @param evloop Event loop
 * @return Number of events processed
 */
int native_evloop_run_once(native_evloop_t* evloop);

/**
 * Register FD with event loop
 * @param evloop Event loop
 * @param fd File descriptor
 * @param events Events to monitor (EPOLLIN, EPOLLOUT)
 * @param user_data User data for callback
 * @return 0 on success, -1 on error
 */
int native_evloop_register(native_evloop_t* evloop, int fd, uint32_t events, void* user_data);

/**
 * Unregister FD from event loop
 * @param evloop Event loop
 * @param fd File descriptor
 */
void native_evloop_unregister(native_evloop_t* evloop, int fd);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_IO_H */
