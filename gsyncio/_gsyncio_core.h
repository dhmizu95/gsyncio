#ifndef _GSYNCIO_CORE_H
#define _GSYNCIO_CORE_H

#include <Python.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

/* Fiber types - unified for both models */
typedef enum {
    FIBER_SYNC,      /* Sync function (PyCFunction wrapper) */
    FIBER_ASYNC      /* Async coroutine */
} FiberType;

/* Fiber states */
typedef enum {
    FIBER_STATE_NEW,
    FIBER_STATE_RUNNING,
    FIBER_STATE_SUSPENDED,
    FIBER_STATE_DONE,
    FIBER_STATE_CANCELLED
} FiberState;

/* Fiber object - unified for both sync and async */
typedef struct Fiber {
    PyObject_HEAD
    
    /* Common fields */
    FiberType type;              /* SYNC or ASYNC */
    FiberState state;            /* Current state */
    int id;                      /* Unique fiber ID */
    int pending;                 /* 1 if in pending queue */
    int ready;                   /* 1 if in ready queue */
    
    /* For async fibers */
    PyObject *coro;              /* Python coroutine (ASYNC only) */
    PyObject *awaited;           /* Current awaitable (ASYNC only) */
    
    /* For sync fibers */
    PyCFunction func;            /* C function (SYNC only) */
    PyObject *args;              /* Function args (SYNC only) */
    
    /* Execution results */
    PyObject *result;            /* Return value */
    PyObject *exception;         /* Exception if failed */
    PyObject *select_state;      /* SelectStateObject if waiting */
    
    /* Thread state for GIL management */
    PyThreadState *tstate;
    
    /* Linked list pointers for queues */
    struct Fiber *next;
    struct Fiber *prev;
} FiberObject;

/* Select wait node for channel queues */
typedef struct SelectWaitNode {
    struct Fiber *fiber;
    struct SelectState *select_state;
    int case_index;
    struct SelectWaitNode *next;
    struct SelectWaitNode *prev;
} SelectWaitNode;

/* Select case types */
typedef enum {
    SELECT_RECV,
    SELECT_SEND,
    SELECT_DEFAULT
} SelectCaseType;

/* Select case */
typedef struct SelectCase {
    SelectCaseType type;
    PyObject *channel;           /* ChannelObject */
    PyObject *value;             /* If send, value to send */
    SelectWaitNode *node;        /* Node registered in channel queue */
} SelectCase;

/* Select object */
typedef struct SelectState {
    PyObject_HEAD
    SelectCase *cases;
    SelectWaitNode *nodes;       /* Waiting nodes, one per case */
    int case_count;
    int done_index;              /* Index of ready case */
    PyObject *result_value;
    struct Fiber *fiber;         /* Waiting fiber */
} SelectStateObject;

/* Future object (C implementation) */
typedef struct Future {
    PyObject_HEAD
    
    struct Fiber *fiber;         /* Fiber waiting on this future */
    PyObject *result;            /* Result value */
    PyObject *exception;         /* Exception if failed */
    PyObject *callbacks;         /* List of callbacks */
    int done;                    /* Completion flag */
} FutureObject;

/* Timer for sleep/wakeup */
typedef struct Timer {
    int64_t deadline_ns;         /* Nanoseconds since epoch */
    FiberObject *fiber;          /* Fiber to wake */
    struct Timer *next;
} TimerObject;

/* Event loop (per-thread) */
typedef struct EventLoop {
    PyObject_HEAD
    
    FiberObject *ready_queue;    /* Fibers ready to run */
    FiberObject *pending_queue;  /* Fibers waiting on awaitables */
    FiberObject *main_fiber;     /* Main coroutine fiber */
    
    TimerObject *timer_queue;    /* Scheduled timers */
    
    pthread_mutex_t lock;        /* Queue protection */
    pthread_cond_t cond;         /* Wait condition */
    
    int running;                 /* Loop is running */
    int fiber_count;             /* Active fiber count */
    int pending_count;           /* Pending fiber count */
} EventLoopObject;

/* Externs for types */
extern PyTypeObject FiberType_Obj;
extern PyTypeObject FutureType_Obj;
extern PyTypeObject EventLoopType_Obj;
extern PyTypeObject ChannelType_Obj;

#endif /* _GSYNCIO_CORE_H */
