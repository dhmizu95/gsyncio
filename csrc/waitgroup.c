/**
 * waitgroup.c - WaitGroup implementation for gsyncio
 * 
 * Synchronization primitive for waiting on multiple fiber completions
 * (like Go's sync.WaitGroup).
 */

#include "waitgroup.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

waitgroup_t* waitgroup_create(void) {
    waitgroup_t* wg = (waitgroup_t*)calloc(1, sizeof(waitgroup_t));
    if (!wg) {
        return NULL;
    }
    
    wg->counter = 0;
    wg->waiting_fibers = NULL;
    wg->refcount = 1;
    
    pthread_mutex_init(&wg->mutex, NULL);
    pthread_cond_init(&wg->cond, NULL);
    
    return wg;
}

void waitgroup_destroy(waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    
    pthread_mutex_destroy(&wg->mutex);
    pthread_cond_destroy(&wg->cond);
    
    free(wg);
}

void waitgroup_incref(waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    
    pthread_mutex_lock(&wg->mutex);
    wg->refcount++;
    pthread_mutex_unlock(&wg->mutex);
}

void waitgroup_decref(waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    
    pthread_mutex_lock(&wg->mutex);
    wg->refcount--;
    if (wg->refcount == 0) {
        pthread_mutex_unlock(&wg->mutex);
        waitgroup_destroy(wg);
    } else {
        pthread_mutex_unlock(&wg->mutex);
    }
}

int waitgroup_add(waitgroup_t* wg, int64_t delta) {
    if (!wg) {
        return -1;
    }
    
    pthread_mutex_lock(&wg->mutex);
    
    wg->counter += delta;
    
    if (wg->counter < 0) {
        /* Negative counter is an error */
        fprintf(stderr, "waitgroup: negative counter\n");
        pthread_mutex_unlock(&wg->mutex);
        return -1;
    }
    
    if (wg->counter == 0) {
        /* Wake up all waiting fibers */
        fiber_t* waiting = wg->waiting_fibers;
        wg->waiting_fibers = NULL;
        
        pthread_cond_broadcast(&wg->cond);
        pthread_mutex_unlock(&wg->mutex);
        
        /* Resume waiting fibers */
        while (waiting) {
            fiber_t* next = waiting->next_ready;
            fiber_unpark(waiting);
            waiting = next;
        }
    } else {
        pthread_mutex_unlock(&wg->mutex);
    }
    
    return 0;
}

void waitgroup_done(waitgroup_t* wg) {
    waitgroup_add(wg, -1);
}

void waitgroup_wait(waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    
    pthread_mutex_lock(&wg->mutex);
    
    /* Check if counter is already zero */
    if (wg->counter == 0) {
        pthread_mutex_unlock(&wg->mutex);
        return;
    }
    
    /* Get current fiber */
    fiber_t* current = fiber_current();
    if (!current) {
        /* Not in fiber context - use blocking wait */
        while (wg->counter > 0) {
            pthread_cond_wait(&wg->cond, &wg->mutex);
        }
        pthread_mutex_unlock(&wg->mutex);
        return;
    }
    
    /* Add to waiting list */
    current->next_ready = wg->waiting_fibers;
    wg->waiting_fibers = current;
    
    pthread_mutex_unlock(&wg->mutex);
    
    /* Park the fiber */
    fiber_park();
}

int64_t waitgroup_counter(waitgroup_t* wg) {
    if (!wg) {
        return 0;
    }
    
    pthread_mutex_lock(&wg->mutex);
    int64_t counter = wg->counter;
    pthread_mutex_unlock(&wg->mutex);
    
    return counter;
}
