/**
 * channel.c - Channel implementation for gsyncio
 * 
 * Typed message-passing channels between fibers (like Go channels).
 * Supports buffered and unbuffered channels.
 */

#include "channel.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Global channel ID counter */
static uint64_t g_channel_id_counter = 0;

channel_t* channel_create(size_t capacity) {
    channel_t* ch = (channel_t*)calloc(1, sizeof(channel_t));
    if (!ch) {
        return NULL;
    }
    
    ch->id = g_channel_id_counter++;
    ch->capacity = capacity;
    ch->size = 0;
    ch->head = NULL;
    ch->tail = NULL;
    ch->closed = false;
    ch->send_waiters = NULL;
    ch->recv_waiters = NULL;
    ch->refcount = 1;
    
    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->cond, NULL);
    
    return ch;
}

void channel_destroy(channel_t* ch) {
    if (!ch) {
        return;
    }
    
    /* Free buffered items */
    channel_item_t* item = ch->head;
    while (item) {
        channel_item_t* next = item->next;
        /* Note: value is a Python object, should be DECREF'd by caller */
        free(item);
        item = next;
    }
    
    pthread_mutex_destroy(&ch->mutex);
    pthread_cond_destroy(&ch->cond);
    
    free(ch);
}

void channel_incref(channel_t* ch) {
    if (!ch) {
        return;
    }
    
    pthread_mutex_lock(&ch->mutex);
    ch->refcount++;
    pthread_mutex_unlock(&ch->mutex);
}

void channel_decref(channel_t* ch) {
    if (!ch) {
        return;
    }
    
    pthread_mutex_lock(&ch->mutex);
    ch->refcount--;
    if (ch->refcount == 0) {
        pthread_mutex_unlock(&ch->mutex);
        channel_destroy(ch);
    } else {
        pthread_mutex_unlock(&ch->mutex);
    }
}

/* Internal: create a new channel item */
static channel_item_t* channel_item_create(void* value) {
    channel_item_t* item = (channel_item_t*)calloc(1, sizeof(channel_item_t));
    if (!item) {
        return NULL;
    }
    item->value = value;
    item->next = NULL;
    return item;
}

/* Internal: enqueue item to buffer */
static void channel_enqueue(channel_t* ch, channel_item_t* item) {
    item->next = NULL;
    if (ch->tail) {
        ch->tail->next = item;
    } else {
        ch->head = item;
    }
    ch->tail = item;
    ch->size++;
}

/* Internal: dequeue item from buffer */
static channel_item_t* channel_dequeue(channel_t* ch) {
    if (!ch->head) {
        return NULL;
    }
    
    channel_item_t* item = ch->head;
    ch->head = item->next;
    if (!ch->head) {
        ch->tail = NULL;
    }
    ch->size--;
    item->next = NULL;
    
    return item;
}

/* Internal: wake up a waiting sender */
static void channel_wake_sender(channel_t* ch, void* value) {
    fiber_t* waiter = ch->send_waiters;
    if (!waiter) {
        return;
    }
    
    /* Remove from waiters list */
    ch->send_waiters = waiter->next_ready;
    
    /* Set result and resume */
    waiter->result = NULL;  /* Send succeeded */
    fiber_unpark(waiter);
}

/* Internal: wake up a waiting receiver */
static void* channel_wake_receiver(channel_t* ch) {
    fiber_t* waiter = ch->recv_waiters;
    if (!waiter) {
        return NULL;
    }
    
    /* Remove from waiters list */
    ch->recv_waiters = waiter->next_ready;
    
    /* Resume fiber */
    fiber_unpark(waiter);
    
    return waiter->result;
}

int channel_send(channel_t* ch, void* value) {
    if (!ch || ch->closed) {
        return -1;
    }
    
    pthread_mutex_lock(&ch->mutex);
    
    /* Check if there's a waiting receiver (unbuffered or buffer empty) */
    if (ch->recv_waiters) {
        /* Direct handoff to receiver */
        fiber_t* waiter = ch->recv_waiters;
        ch->recv_waiters = waiter->next_ready;
        waiter->result = value;
        fiber_unpark(waiter);
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    
    /* If buffered and not full, enqueue */
    if (ch->capacity > 0 && ch->size < ch->capacity) {
        channel_item_t* item = channel_item_create(value);
        if (!item) {
            pthread_mutex_unlock(&ch->mutex);
            return -1;
        }
        channel_enqueue(ch, item);
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    
    /* Buffer full or unbuffered - need to wait */
    fiber_t* current = fiber_current();
    if (!current) {
        pthread_mutex_unlock(&ch->mutex);
        return -1;  /* Not in fiber context */
    }
    
    /* Add to send waiters */
    current->result = value;
    current->next_ready = ch->send_waiters;
    ch->send_waiters = current;
    
    pthread_mutex_unlock(&ch->mutex);
    
    /* Park the fiber */
    fiber_park();
    
    /* Check if channel was closed while waiting */
    if (ch->closed) {
        return -1;
    }
    
    return 0;
}

void* channel_recv(channel_t* ch) {
    if (!ch) {
        return NULL;
    }
    
    pthread_mutex_lock(&ch->mutex);
    
    /* Check if there's buffered data */
    if (ch->head) {
        channel_item_t* item = channel_dequeue(ch);
        void* value = item->value;
        free(item);
        pthread_mutex_unlock(&ch->mutex);
        return value;
    }
    
    /* Check if there's a waiting sender */
    if (ch->send_waiters) {
        fiber_t* waiter = ch->send_waiters;
        ch->send_waiters = waiter->next_ready;
        void* value = waiter->result;
        waiter->result = NULL;
        fiber_unpark(waiter);
        pthread_mutex_unlock(&ch->mutex);
        return value;
    }
    
    /* Channel closed and empty */
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return NULL;
    }
    
    /* Need to wait for data */
    fiber_t* current = fiber_current();
    if (!current) {
        pthread_mutex_unlock(&ch->mutex);
        return NULL;  /* Not in fiber context */
    }
    
    /* Add to recv waiters */
    current->next_ready = ch->recv_waiters;
    ch->recv_waiters = current;
    
    pthread_mutex_unlock(&ch->mutex);
    
    /* Park the fiber */
    fiber_park();
    
    /* Return value set by sender */
    return current->result;
}

int channel_try_send(channel_t* ch, void* value) {
    if (!ch || ch->closed) {
        return -1;
    }
    
    pthread_mutex_lock(&ch->mutex);
    
    /* Check if there's a waiting receiver */
    if (ch->recv_waiters) {
        fiber_t* waiter = ch->recv_waiters;
        ch->recv_waiters = waiter->next_ready;
        waiter->result = value;
        fiber_unpark(waiter);
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    
    /* If buffered and not full, enqueue */
    if (ch->capacity > 0 && ch->size < ch->capacity) {
        channel_item_t* item = channel_item_create(value);
        if (!item) {
            pthread_mutex_unlock(&ch->mutex);
            return -1;
        }
        channel_enqueue(ch, item);
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    
    /* Would block */
    pthread_mutex_unlock(&ch->mutex);
    return -1;
}

int channel_try_recv(channel_t* ch, void** out_value) {
    if (!ch || !out_value) {
        return -1;
    }
    
    pthread_mutex_lock(&ch->mutex);
    
    /* Check if there's buffered data */
    if (ch->head) {
        channel_item_t* item = channel_dequeue(ch);
        *out_value = item->value;
        free(item);
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    
    /* Check if there's a waiting sender */
    if (ch->send_waiters) {
        fiber_t* waiter = ch->send_waiters;
        ch->send_waiters = waiter->next_ready;
        *out_value = waiter->result;
        waiter->result = NULL;
        fiber_unpark(waiter);
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    
    /* Would block */
    pthread_mutex_unlock(&ch->mutex);
    return -1;
}

int channel_close(channel_t* ch) {
    if (!ch) {
        return -1;
    }
    
    pthread_mutex_lock(&ch->mutex);
    
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return -1;  /* Already closed */
    }
    
    ch->closed = true;
    
    /* Wake up all waiting receivers with NULL */
    while (ch->recv_waiters) {
        fiber_t* waiter = ch->recv_waiters;
        ch->recv_waiters = waiter->next_ready;
        waiter->result = NULL;
        fiber_unpark(waiter);
    }
    
    /* Wake up all waiting senders (they'll get error) */
    while (ch->send_waiters) {
        fiber_t* waiter = ch->send_waiters;
        ch->send_waiters = waiter->next_ready;
        fiber_unpark(waiter);
    }
    
    pthread_mutex_unlock(&ch->mutex);
    
    return 0;
}

bool channel_is_closed(channel_t* ch) {
    if (!ch) {
        return false;
    }
    
    pthread_mutex_lock(&ch->mutex);
    bool closed = ch->closed;
    pthread_mutex_unlock(&ch->mutex);
    
    return closed;
}

size_t channel_size(channel_t* ch) {
    if (!ch) {
        return 0;
    }
    
    pthread_mutex_lock(&ch->mutex);
    size_t size = ch->size;
    pthread_mutex_unlock(&ch->mutex);
    
    return size;
}

bool channel_is_empty(channel_t* ch) {
    return channel_size(ch) == 0;
}

bool channel_is_full(channel_t* ch) {
    if (!ch || ch->capacity == 0) {
        return false;
    }
    
    pthread_mutex_lock(&ch->mutex);
    bool full = (ch->size >= ch->capacity);
    pthread_mutex_unlock(&ch->mutex);
    
    return full;
}

uint64_t channel_id(channel_t* ch) {
    return ch ? ch->id : 0;
}
