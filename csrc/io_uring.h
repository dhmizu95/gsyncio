#ifndef IOURING_H
#define IOURING_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef __linux__

#include <linux/io_uring.h>

typedef struct io_uring_s {
    int fd;
    uint32_t ring_size;
    struct io_uring_params params;
    void *sq_ptr;
    void *cq_ptr;
    uint32_t *sq_head;
    uint32_t *sq_tail;
    uint32_t *cq_head;
    uint32_t *cq_tail;
    struct io_uring_sqe *sqes;
    uint32_t sq_mask;
    uint32_t cq_mask;
} io_uring_t;

int io_uring_init(io_uring_t *ring, uint32_t entries);
void io_uring_destroy(io_uring_t *ring);
int io_uring_submit(io_uring_t *ring);
int io_uring_wait_cqe(io_uring_t *ring, struct io_uring_cqe **cqe);
int io_uring_peek_cqe(io_uring_t *ring, struct io_uring_cqe **cqe);
void io_uring_cqe_seen(io_uring_t *ring, struct io_uring_cqe *cqe);

struct io_uring_sqe *io_uring_get_sqe(io_uring_t *ring);

int io_uring_read(io_uring_t *ring, int fd, void *buf, uint64_t nbytes, uint64_t offset, uint64_t user_data);
int io_uring_write(io_uring_t *ring, int fd, const void *buf, uint64_t nbytes, uint64_t offset, uint64_t user_data);
int io_uring_accept(io_uring_t *ring, int fd, struct sockaddr *addr, socklen_t *addrlen, uint64_t user_data);
int io_uring_connect(io_uring_t *ring, int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t user_data);
int io_uring_poll_add(io_uring_t *ring, int fd, uint32_t poll_mask, uint64_t user_data);
int io_uring_nop(io_uring_t *ring, uint64_t user_data);
int io_uring_cancel(io_uring_t *ring, uint64_t user_data, uint64_t user_data2);

#define IOURING_READ  1
#define IOURING_WRITE 2

#define IOURING_AVAILABLE 1

#else

#define IOURING_AVAILABLE 0
typedef struct { int dummy; } io_uring_t;

#endif

#endif
