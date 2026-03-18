#ifdef __linux__

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include "io_uring.h"

static int io_uring_supported = -1;

static inline int io_uring_enter(int fd, unsigned int to_submit, unsigned int to_wait, unsigned int flags, void *arg) {
    return syscall(__NR_io_uring_enter, fd, to_submit, to_wait, flags, arg, sizeof(unsigned long));
}

static inline int io_uring_setup(unsigned int entries, struct io_uring_params *p) {
    return syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_check_support(void) {
    if (io_uring_supported == -1) {
        int fd = io_uring_setup(1, &(struct io_uring_params){0});
        if (fd >= 0) {
            close(fd);
            io_uring_supported = 1;
        } else {
            io_uring_supported = 0;
        }
    }
    return io_uring_supported;
}

int io_uring_init(io_uring_t *ring, uint32_t entries) {
    if (!io_uring_check_support()) {
        memset(ring, 0, sizeof(*ring));
        ring->fd = -1;
        return -1;
    }
    memset(ring, 0, sizeof(*ring));
    
    ring->fd = io_uring_setup(entries, &ring->params);
    if (ring->fd < 0) {
        return -1;
    }
    
    size_t sq_ring_size = ring->params.sq_off.array + ring->params.sq_entries * sizeof(__u32);
    size_t cq_ring_size = ring->params.cq_off.cqes + ring->params.cq_entries * sizeof(struct io_uring_cqe);
    
    ring->sq_ptr = mmap(NULL, sq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, IORING_OFF_SQ_RING);
    if (ring->sq_ptr == MAP_FAILED) {
        close(ring->fd);
        return -1;
    }
    
    ring->cq_ptr = mmap(NULL, cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, IORING_OFF_CQ_RING);
    if (ring->cq_ptr == MAP_FAILED) {
        munmap(ring->sq_ptr, sq_ring_size);
        close(ring->fd);
        return -1;
    }
    
    ring->sq_head = (uint32_t *)((char *)ring->sq_ptr + ring->params.sq_off.head);
    ring->sq_tail = (uint32_t *)((char *)ring->sq_ptr + ring->params.sq_off.tail);
    ring->cq_head = (uint32_t *)((char *)ring->cq_ptr + ring->params.cq_off.head);
    ring->cq_tail = (uint32_t *)((char *)ring->cq_ptr + ring->params.cq_off.tail);
    ring->sq_mask = ring->params.sq_entries - 1;
    ring->cq_mask = ring->params.cq_entries - 1;
    
    ring->sqes = mmap(NULL, ring->params.sq_entries * sizeof(struct io_uring_sqe),
                     PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, IORING_OFF_SQES);
    if (ring->sqes == MAP_FAILED) {
        munmap(ring->cq_ptr, cq_ring_size);
        munmap(ring->sq_ptr, sq_ring_size);
        close(ring->fd);
        return -1;
    }
    
    ring->ring_size = entries;
    return 0;
}

void io_uring_destroy(io_uring_t *ring) {
    if (!ring || ring->fd < 0) return;
    
    size_t sq_ring_size = ring->params.sq_off.array + ring->params.sq_entries * sizeof(__u32);
    size_t cq_ring_size = ring->params.cq_off.cqes + ring->params.cq_entries * sizeof(struct io_uring_cqe);
    
    munmap(ring->sqes, ring->params.sq_entries * sizeof(struct io_uring_sqe));
    munmap(ring->cq_ptr, cq_ring_size);
    munmap(ring->sq_ptr, sq_ring_size);
    close(ring->fd);
    ring->fd = -1;
}

int io_uring_submit(io_uring_t *ring) {
    if (!ring || ring->fd < 0) return -1;
    return io_uring_enter(ring->fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
}

struct io_uring_sqe *io_uring_get_sqe(io_uring_t *ring) {
    uint32_t tail = *ring->sq_tail;
    uint32_t head = *ring->sq_head;
    
    if ((tail - head) >= ring->params.sq_entries) {
        return NULL;
    }
    
    struct io_uring_sqe *sqe = &ring->sqes[tail & ring->sq_mask];
    *ring->sq_tail = tail + 1;
    return sqe;
}

int io_uring_wait_cqe(io_uring_t *ring, struct io_uring_cqe **cqe) {
    if (!ring || ring->fd < 0) return -1;
    
    int ret = io_uring_enter(ring->fd, 1, 1, IORING_ENTER_GETEVENTS, NULL);
    if (ret < 0) return ret;
    
    return io_uring_peek_cqe(ring, cqe);
}

int io_uring_peek_cqe(io_uring_t *ring, struct io_uring_cqe **cqe) {
    if (!ring || ring->fd < 0) return -1;
    
    uint32_t head = *ring->cq_head;
    if (head == *ring->cq_tail) {
        *cqe = NULL;
        return 0;
    }
    
    *cqe = (struct io_uring_cqe *)((char *)ring->cq_ptr + ring->params.cq_off.cqes) + (head & ring->cq_mask);
    return 1;
}

void io_uring_cqe_seen(io_uring_t *ring, struct io_uring_cqe *cqe) {
    (void)ring;
    (void)cqe;
    *ring->cq_head = (*ring->cq_head + 1);
}

int io_uring_read(io_uring_t *ring, int fd, void *buf, uint64_t nbytes, uint64_t offset, uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (uint64_t)buf;
    sqe->len = nbytes;
    sqe->off = offset;
    sqe->user_data = user_data;
    return 0;
}

int io_uring_write(io_uring_t *ring, int fd, const void *buf, uint64_t nbytes, uint64_t offset, uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)buf;
    sqe->len = nbytes;
    sqe->off = offset;
    sqe->user_data = user_data;
    return 0;
}

int io_uring_accept(io_uring_t *ring, int fd, struct sockaddr *addr, socklen_t *addrlen, uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = fd;
    sqe->addr = (uint64_t)addr;
    sqe->len = *addrlen;
    sqe->user_data = user_data;
    return 0;
}

int io_uring_connect(io_uring_t *ring, int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = fd;
    sqe->addr = (uint64_t)addr;
    sqe->len = addrlen;
    sqe->user_data = user_data;
    return 0;
}

int io_uring_poll_add(io_uring_t *ring, int fd, uint32_t poll_mask, uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = poll_mask;
    sqe->user_data = user_data;
    return 0;
}

int io_uring_nop(io_uring_t *ring, uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = user_data;
    return 0;
}

int io_uring_cancel(io_uring_t *ring, uint64_t user_data, uint64_t user_data2) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = user_data;
    sqe->user_data = user_data2;
    return 0;
}

#endif