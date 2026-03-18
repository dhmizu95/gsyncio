/**
 * select.h - Select statement interface for gsyncio
 * 
 * Multiplexing on channel operations (like Go's select statement).
 */

#ifndef SELECT_H
#define SELECT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Select case types */
typedef enum select_case_type {
    SELECT_CASE_RECV,     /* Receive case: recv(chan) */
    SELECT_CASE_SEND,     /* Send case: send(chan, value) */
    SELECT_CASE_DEFAULT   /* Default case: non-blocking */
} select_case_type_t;

/* Select case structure */
typedef struct select_case {
    select_case_type_t type;    /* Case type */
    channel_t* channel;         /* Channel for operation */
    void* value;                /* Value to send (for send cases) */
    void* recv_value;           /* Received value (output for recv cases) */
    bool executed;              /* Was this case executed? */
} select_case_t;

/* Select result */
typedef struct select_result {
    int case_index;             /* Index of executed case (-1 if none) */
    select_case_t* case_info;   /* Pointer to executed case */
    void* value;                /* Value (received value or NULL) */
    bool success;               /* Was operation successful? */
} select_result_t;

/* Select state for multi-step operations */
typedef struct select_state {
    select_case_t* cases;       /* Array of cases */
    size_t case_count;          /* Number of cases */
    select_result_t result;     /* Result */
    
    /* Waiting fibers */
    void* waiting_fiber;        /* Fiber waiting on select */
    
    /* Reference counting */
    int refcount;
    
    /* Synchronization */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} select_state_t;

/* Select API */

/**
 * Create a select state
 * @param case_count Number of cases
 * @return Select state, or NULL on error
 */
select_state_t* select_create(size_t case_count);

/**
 * Destroy a select state
 * @param sel Select state
 */
void select_destroy(select_state_t* sel);

/**
 * Set up a receive case
 * @param sel Select state
 * @param index Case index
 * @param ch Channel to receive from
 */
void select_set_recv(select_state_t* sel, size_t index, channel_t* ch);

/**
 * Set up a send case
 * @param sel Select state
 * @param index Case index
 * @param ch Channel to send to
 * @param value Value to send
 */
void select_set_send(select_state_t* sel, size_t index, channel_t* ch, void* value);

/**
 * Set up a default case (non-blocking)
 * @param sel Select state
 * @param index Case index
 */
void select_set_default(select_state_t* sel, size_t index);

/**
 * Execute select (blocks until a case is ready)
 * @param sel Select state
 * @return Select result
 */
select_result_t select_execute(select_state_t* sel);

/**
 * Try select without blocking
 * @param sel Select state
 * @return Select result
 */
select_result_t select_try(select_state_t* sel);

/**
 * Get select result
 * @param sel Select state
 * @return Select result
 */
select_result_t select_result(select_state_t* sel);

#ifdef __cplusplus
}
#endif

#endif /* SELECT_H */
