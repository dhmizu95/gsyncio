/**
 * select.c - Select statement implementation for gsyncio
 * 
 * Multiplexing on channel operations (like Go's select statement).
 */

#include "select.h"
#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

select_state_t* select_create(size_t case_count) {
    if (case_count == 0) {
        return NULL;
    }
    
    select_state_t* sel = (select_state_t*)calloc(1, sizeof(select_state_t));
    if (!sel) {
        return NULL;
    }
    
    sel->cases = (select_case_t*)calloc(case_count, sizeof(select_case_t));
    if (!sel->cases) {
        free(sel);
        return NULL;
    }
    
    sel->case_count = case_count;
    sel->result.case_index = -1;
    sel->result.case_info = NULL;
    sel->result.value = NULL;
    sel->result.success = false;
    sel->waiting_fiber = NULL;
    sel->refcount = 1;
    
    pthread_mutex_init(&sel->mutex, NULL);
    pthread_cond_init(&sel->cond, NULL);
    
    return sel;
}

void select_destroy(select_state_t* sel) {
    if (!sel) {
        return;
    }
    
    if (sel->cases) {
        free(sel->cases);
    }
    
    pthread_mutex_destroy(&sel->mutex);
    pthread_cond_destroy(&sel->cond);
    
    free(sel);
}

void select_set_recv(select_state_t* sel, size_t index, channel_t* ch) {
    if (!sel || index >= sel->case_count) {
        return;
    }
    
    select_case_t* c = &sel->cases[index];
    c->type = SELECT_CASE_RECV;
    c->channel = ch;
    c->value = NULL;
    c->recv_value = NULL;
    c->executed = false;
}

void select_set_send(select_state_t* sel, size_t index, channel_t* ch, void* value) {
    if (!sel || index >= sel->case_count) {
        return;
    }
    
    select_case_t* c = &sel->cases[index];
    c->type = SELECT_CASE_SEND;
    c->channel = ch;
    c->value = value;
    c->recv_value = NULL;
    c->executed = false;
}

void select_set_default(select_state_t* sel, size_t index) {
    if (!sel || index >= sel->case_count) {
        return;
    }
    
    select_case_t* c = &sel->cases[index];
    c->type = SELECT_CASE_DEFAULT;
    c->channel = NULL;
    c->value = NULL;
    c->recv_value = NULL;
    c->executed = false;
}

/* Internal: try to execute a single case without blocking */
static int select_try_case(select_state_t* sel, size_t index) {
    select_case_t* c = &sel->cases[index];
    
    switch (c->type) {
        case SELECT_CASE_RECV:
            if (!c->channel || channel_is_closed(c->channel)) {
                return -1;
            }
            
            /* Try non-blocking receive */
            void* value;
            if (channel_try_recv(c->channel, &value) == 0) {
                c->recv_value = value;
                c->executed = true;
                sel->result.case_index = (int)index;
                sel->result.case_info = c;
                sel->result.value = value;
                sel->result.success = true;
                return 0;
            }
            return -1;
            
        case SELECT_CASE_SEND:
            if (!c->channel || channel_is_closed(c->channel)) {
                return -1;
            }
            
            /* Try non-blocking send */
            if (channel_try_send(c->channel, c->value) == 0) {
                c->executed = true;
                sel->result.case_index = (int)index;
                sel->result.case_info = c;
                sel->result.value = NULL;
                sel->result.success = true;
                return 0;
            }
            return -1;
            
        case SELECT_CASE_DEFAULT:
            /* Default always succeeds if no other case is ready */
            c->executed = true;
            sel->result.case_index = (int)index;
            sel->result.case_info = c;
            sel->result.value = NULL;
            sel->result.success = true;
            return 0;
            
        default:
            return -1;
    }
}

select_result_t select_try(select_state_t* sel) {
    select_result_t no_result = {-1, NULL, NULL, false};
    
    if (!sel) {
        return no_result;
    }
    
    pthread_mutex_lock(&sel->mutex);
    
    /* Reset result */
    sel->result.case_index = -1;
    sel->result.case_info = NULL;
    sel->result.value = NULL;
    sel->result.success = false;
    
    /* First, check if any case can proceed immediately */
    for (size_t i = 0; i < sel->case_count; i++) {
        if (sel->cases[i].type != SELECT_CASE_DEFAULT) {
            if (select_try_case(sel, i) == 0) {
                pthread_mutex_unlock(&sel->mutex);
                return sel->result;
            }
        }
    }
    
    /* Check for default case */
    for (size_t i = 0; i < sel->case_count; i++) {
        if (sel->cases[i].type == SELECT_CASE_DEFAULT) {
            select_try_case(sel, i);
            pthread_mutex_unlock(&sel->mutex);
            return sel->result;
        }
    }
    
    pthread_mutex_unlock(&sel->mutex);
    return no_result;
}

select_result_t select_execute(select_state_t* sel) {
    if (!sel) {
        select_result_t no_result = {-1, NULL, NULL, false};
        return no_result;
    }
    
    /* First try without blocking */
    select_result_t result = select_try(sel);
    if (result.success) {
        return result;
    }
    
    /* No case ready immediately - need to wait */
    pthread_mutex_lock(&sel->mutex);
    
    /* Get current fiber */
    fiber_t* current = fiber_current();
    if (!current) {
        /* Not in fiber context - spin wait (not ideal but works) */
        pthread_mutex_unlock(&sel->mutex);
        
        while (1) {
            result = select_try(sel);
            if (result.success) {
                return result;
            }
            
            /* Brief sleep to avoid busy-waiting */
            fiber_yield();
        }
    }
    
    /* Register fiber as waiting on all channels */
    for (size_t i = 0; i < sel->case_count; i++) {
        select_case_t* c = &sel->cases[i];
        if (c->type == SELECT_CASE_RECV && c->channel) {
            /* Add to channel's recv waiters */
            /* This is simplified - full implementation would track select state */
        } else if (c->type == SELECT_CASE_SEND && c->channel) {
            /* Add to channel's send waiters */
        }
    }
    
    sel->waiting_fiber = current;
    
    pthread_mutex_unlock(&sel->mutex);
    
    /* Park the fiber */
    fiber_park();
    
    /* Return result set by channel operation */
    return sel->result;
}

select_result_t select_result(select_state_t* sel) {
    if (!sel) {
        select_result_t no_result = {-1, NULL, NULL, false};
        return no_result;
    }
    
    return sel->result;
}
