#include <Python.h>
#include "future.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations for scheduler functions */
extern void fiber_park(void);
extern void fiber_unpark(fiber_t* fiber);
extern fiber_t* fiber_current(void);

future_t* future_create(void) {
    future_t* f = (future_t*)calloc(1, sizeof(future_t));
    if (!f) {
        return NULL;
    }
    
    f->state = FUTURE_PENDING;
    f->result = NULL;
    f->exception = NULL;
    f->callbacks = NULL;
    f->callback_count = 0;
    f->callback_capacity = 0;
    f->waiting_fibers = NULL;
    f->refcount = 1;
    
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->cond, NULL);
    
    return f;
}

void future_destroy(future_t* f) {
    if (!f) {
        return;
    }
    
    /* Free callbacks */
    if (f->callbacks) {
        free(f->callbacks);
    }
    
    /* Note: result and exception are Python objects, 
       they should be DECREF'd by the caller */
    
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
    
    free(f);
}

void future_incref(future_t* f) {
    if (!f) {
        return;
    }
    
    pthread_mutex_lock(&f->mutex);
    f->refcount++;
    pthread_mutex_unlock(&f->mutex);
}

void future_decref(future_t* f) {
    if (!f) {
        return;
    }
    
    pthread_mutex_lock(&f->mutex);
    f->refcount--;
    if (f->refcount == 0) {
        pthread_mutex_unlock(&f->mutex);
        future_destroy(f);
    } else {
        pthread_mutex_unlock(&f->mutex);
    }
}

bool future_is_done(future_t* f) {
    if (!f) {
        return false;
    }
    
    pthread_mutex_lock(&f->mutex);
    bool done = (f->state != FUTURE_PENDING);
    pthread_mutex_unlock(&f->mutex);
    
    return done;
}

bool future_has_exception(future_t* f) {
    if (!f) {
        return false;
    }
    
    pthread_mutex_lock(&f->mutex);
    bool has_exc = (f->state == FUTURE_EXCEPTION);
    pthread_mutex_unlock(&f->mutex);
    
    return has_exc;
}

int future_set_result(future_t* f, void* result) {
    if (!f) {
        return -1;
    }
    
    pthread_mutex_lock(&f->mutex);
    
    if (f->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&f->mutex);
        return -1;  /* Already done */
    }
    
    f->state = FUTURE_READY;
    f->result = result;
    
    /* Wake up waiting fibers */
    fiber_t* waiting = f->waiting_fibers;
    f->waiting_fibers = NULL;
    
    /* Invoke callbacks */
    for (size_t i = 0; i < f->callback_count; i++) {
        future_callback_t cb = (future_callback_t)f->callbacks[i];
        if (cb) {
            cb(f, NULL);  /* user_data would be stored with callback */
        }
    }
    
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->mutex);
    
    /* Resume waiting fibers */
    while (waiting) {
        fiber_t* next = waiting->next_ready;
        fiber_unpark(waiting);
        waiting = next;
    }
    
    return 0;
}

void* future_result(future_t* f) {
    if (!f) {
        return NULL;
    }
    
    pthread_mutex_lock(&f->mutex);
    
    while (f->state == FUTURE_PENDING) {
        #ifdef Py_BEGIN_ALLOW_THREADS
        Py_BEGIN_ALLOW_THREADS
        #endif
        
        pthread_cond_wait(&f->cond, &f->mutex);
        
        #ifdef Py_END_ALLOW_THREADS
        Py_END_ALLOW_THREADS
        #endif
    }
    
    void* result = f->result;
    pthread_mutex_unlock(&f->mutex);
    
    return result;
}

int future_result_nowait(future_t* f, void** out_result) {
    if (!f || !out_result) {
        return -1;
    }
    
    pthread_mutex_lock(&f->mutex);
    
    if (f->state == FUTURE_PENDING) {
        pthread_mutex_unlock(&f->mutex);
        return -1;  /* Still pending */
    }
    
    *out_result = f->result;
    pthread_mutex_unlock(&f->mutex);
    
    return 0;
}

int future_set_exception(future_t* f, void* exc) {
    if (!f) {
        return -1;
    }
    
    pthread_mutex_lock(&f->mutex);
    
    if (f->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&f->mutex);
        return -1;  /* Already done */
    }
    
    f->state = FUTURE_EXCEPTION;
    f->exception = exc;
    
    /* Wake up waiting fibers */
    fiber_t* waiting = f->waiting_fibers;
    f->waiting_fibers = NULL;
    
    /* Invoke callbacks */
    for (size_t i = 0; i < f->callback_count; i++) {
        future_callback_t cb = (future_callback_t)f->callbacks[i];
        if (cb) {
            cb(f, NULL);
        }
    }
    
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->mutex);
    
    /* Resume waiting fibers */
    while (waiting) {
        fiber_t* next = waiting->next_ready;
        fiber_unpark(waiting);
        waiting = next;
    }
    
    return 0;
}

void* future_exception(future_t* f) {
    if (!f) {
        return NULL;
    }
    
    pthread_mutex_lock(&f->mutex);
    void* exc = f->exception;
    pthread_mutex_unlock(&f->mutex);
    
    return exc;
}

int future_add_callback(future_t* f, future_callback_t callback, void* user_data) {
    if (!f || !callback) {
        return -1;
    }
    
    pthread_mutex_lock(&f->mutex);
    
    /* If already done, invoke callback immediately */
    if (f->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&f->mutex);
        callback(f, user_data);
        return 0;
    }
    
    /* Grow callback array if needed */
    if (f->callback_count >= f->callback_capacity) {
        size_t new_capacity = f->callback_capacity == 0 ? 4 : f->callback_capacity * 2;
        void** new_callbacks = (void**)realloc(f->callbacks, new_capacity * sizeof(void*));
        if (!new_callbacks) {
            pthread_mutex_unlock(&f->mutex);
            return -1;
        }
        f->callbacks = new_callbacks;
        f->callback_capacity = new_capacity;
    }
    
    /* Store callback and user_data together */
    /* For simplicity, we just store the callback - user_data would need a struct */
    f->callbacks[f->callback_count++] = (void*)callback;
    
    pthread_mutex_unlock(&f->mutex);
    
    return 0;
}

void future_wait(future_t* f) {
    if (!f) {
        return;
    }
    
    future_result(f);  /* Blocks until ready */
}

void future_await(future_t* f) {
    if (!f) {
        return;
    }
    
    pthread_mutex_lock(&f->mutex);
    
    /* Check if already done */
    if (f->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&f->mutex);
        return;
    }
    
    /* Get current fiber */
    fiber_t* current = fiber_current();
    if (!current) {
        pthread_mutex_unlock(&f->mutex);
        return;  /* Not in fiber context */
    }
    
    /* Add to waiting list */
    current->next_ready = f->waiting_fibers;
    f->waiting_fibers = current;
    current->waiting_on = f;
    
    pthread_mutex_unlock(&f->mutex);
    
    /* Park the fiber */
    fiber_park();
}
