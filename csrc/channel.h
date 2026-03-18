/**
 * channel.h - Channel interface for gsyncio
 * 
 * Typed message-passing channels between fibers (like Go channels).
 * Supports buffered and unbuffered channels.
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "fiber.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Channel item (holds Python object) */
typedef struct channel_item {
    void* value;                  /* Python object */
    struct channel_item* next;    /* Next item in queue */
} channel_item_t;

/* Channel structure */
typedef struct channel {
    uint64_t id;                  /* Channel ID */
    size_t capacity;              /* Buffer capacity (0 = unbuffered) */
    size_t size;                  /* Current buffer size */
    
    channel_item_t* head;         /* Head of buffer queue */
    channel_item_t* tail;         /* Tail of buffer queue */
    
    bool closed;                  /* Is channel closed */
    
    /* Waiting fibers */
    fiber_t* send_waiters;     /* Fibers waiting to send */
    fiber_t* recv_waiters;     /* Fibers waiting to receive */
    
    /* Reference counting */
    int refcount;                 /* Reference count */
    
    /* Synchronization */
    pthread_mutex_t mutex;        /* Thread safety */
    pthread_cond_t cond;          /* Condition for waiting */
} channel_t;

/* Channel API */

/**
 * Create a new channel
 * @param capacity Buffer capacity (0 = unbuffered)
 * @return New channel, or NULL on error
 */
channel_t* channel_create(size_t capacity);

/**
 * Destroy a channel
 * @param ch Channel to destroy
 */
void channel_destroy(channel_t* ch);

/**
 * Increment reference count
 * @param ch Channel
 */
void channel_incref(channel_t* ch);

/**
 * Decrement reference count and destroy if zero
 * @param ch Channel
 */
void channel_decref(channel_t* ch);

/**
 * Send value to channel (blocks if buffer full)
 * @param ch Channel
 * @param value Value to send (Python object)
 * @return 0 on success, -1 on error/closed
 */
int channel_send(channel_t* ch, void* value);

/**
 * Receive value from channel (blocks if empty)
 * @param ch Channel
 * @return Value, or NULL on error/closed
 */
void* channel_recv(channel_t* ch);

/**
 * Try to send without blocking
 * @param ch Channel
 * @param value Value to send
 * @return 0 on success, -1 if would block/error
 */
int channel_try_send(channel_t* ch, void* value);

/**
 * Try to receive without blocking
 * @param ch Channel
 * @param out_value Output for value
 * @return 0 on success, -1 if would block/error
 */
int channel_try_recv(channel_t* ch, void** out_value);

/**
 * Close a channel
 * @param ch Channel
 * @return 0 on success, -1 on error
 */
int channel_close(channel_t* ch);

/**
 * Check if channel is closed
 * @param ch Channel
 * @return true if closed
 */
bool channel_is_closed(channel_t* ch);

/**
 * Get channel size (number of buffered items)
 * @param ch Channel
 * @return Number of items in buffer
 */
size_t channel_size(channel_t* ch);

/**
 * Check if channel is empty
 * @param ch Channel
 * @return true if empty
 */
bool channel_is_empty(channel_t* ch);

/**
 * Check if channel is full
 * @param ch Channel
 * @return true if full (for buffered channels)
 */
bool channel_is_full(channel_t* ch);

/**
 * Get channel ID
 * @param ch Channel
 * @return Channel ID
 */
uint64_t channel_id(channel_t* ch);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_H */
