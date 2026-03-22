# Native C Fibers with Async/Await - Implementation Plan

## Overview

This document describes the plan to implement native M:N fiber scheduling in gsyncio with full async/await support, **without depending on asyncio**. All scheduling, fiber management, and event looping will be done in native C code.

**Both Task/Sync and Async/Await models run on the same unified C fiber scheduler.**

---

## Goals

1. **Pure C Event Loop** - No `asyncio.set_event_loop()`, no asyncio dependency
2. **M:N Fiber Scheduling** - M greenlets multiplexed over N worker threads
3. **Native Async/Await** - Coroutines scheduled on gsyncio fibers, not asyncio tasks
4. **Backward Compatible** - Existing Task/Sync model continues to work
5. **High Performance** - Fiber park/unpark in C, minimal Python overhead
6. **Unified Scheduler** - Both sync and async tasks run on same fiber pool

---

## Architecture

### Unified Scheduler - Both Models on Same Fiber Pool

```
┌─────────────────────────────────────────────────────────────────────┐
│                         User Code                                    │
│  Task/Sync: gs.task(worker, arg)  │  Async: async def main()        │
│  gs.sync()                      │         await gs.send(ch, data)   │
└───────────────────────────────────┼─────────────────────────────────┘
                                    │
                    ┌───────────────┴───────────────┐
                    │                               │
                    ▼                               ▼
        ┌───────────────────┐           ┌───────────────────┐
        │   Sync Function   │           │  Async Coroutine  │
        │   (no await)      │           │  (has await)      │
        └───────────────────┘           └───────────────────┘
                    │                               │
                    │  Both become Fibers           │
                    │                               │
                    └───────────────┬───────────────┘
                                    ▼
        ┌───────────────────────────────────────────────────────────────┐
        │              Unified C Fiber Scheduler                         │
        │  ┌─────────────────────────────────────────────────────────┐  │
        │  │  Ready Queue: [Sync Fiber] [Async Fiber] [Sync Fiber]   │  │
        │  │  Pending Queue: [Async waiting on await]                │  │
        │  │  Timer Queue: [sleep callbacks]                         │  │
        │  └─────────────────────────────────────────────────────────┘  │
        └───────────────────────────────────────────────────────────────┘
                                    │
            ┌───────────────────────┼───────────────────────┐
            ▼                       ▼                       ▼
      ┌──────────┐          ┌──────────┐          ┌──────────┐
      │ Fiber 1  │          │ Fiber 2  │          │ Fiber 3  │
      │ (sync)   │          │ (async)  │          │ (sync)   │
      └──────────┘          └──────────┘          └──────────┘
            │                       │                       │
            └───────────────────────┼───────────────────────┘
                                    ▼
        ┌─────────────────────────────────────────┐
        │      C Scheduler (N worker threads)     │
        │  - Work-stealing between workers        │
        │  - pthread_cond_wait for parking        │
        │  - Lock-free atomic task counting       │
        └─────────────────────────────────────────┘
```

### Key Design: Both Models Share Same Infrastructure

| Feature | Task/Sync | Async/Await | Shared Implementation |
|---------|-----------|-------------|----------------------|
| Task Creation | `gs.task(fn, args)` | `async def + await` | Both create C Fibers |
| Execution | Direct C function call | Coroutine send() | Same `fiber_resume()` |
| Blocking | `ch.send()` parks fiber | `await ch.send()` parks fiber | Same `fiber_park()` |
| Waiting | `gs.sync()` | `await gather()` | Same pending queue |
| Channel | `ch.send()/recv()` | `await ch.send_async()` | Same C channel |
| Sleep | `gs.sleep_ms()` | `await gs.sleep()` | Same C timer queue |

---

## Architecture Diagram

```
┌───────────────────────────────────────────────────────────────┐
│                    Python User Code                            │
│  async def main(): await gs.send(ch, data)                    │
│  gs.task(worker, arg)  │  gs.run(main())                      │
└───────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────┐
│              Python Wrapper Layer                              │
│  - gs.run(), gs.send(), gs.recv(), gs.sleep()                 │
│  - Thin wrappers around C functions                            │
│  - No asyncio imports                                          │
└───────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────┐
│              Native C Event Loop                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  - Fiber ready queue (runnable fibers)                  │  │
│  │  - Pending queue (fibers waiting on awaitables)         │  │
│  │  - Timer queue (scheduled wakeups)                      │  │
│  │  - Future management                                    │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
  ┌──────────┐        ┌──────────┐        ┌──────────┐
  │ Fiber 1  │        │ Fiber 2  │        │ Fiber 3  │
  │ (coro A) │        │ (coro B) │        │ (coro C) │
  └──────────┘        └──────────┘        └──────────┘
        │                   │                   │
        └───────────────────┼───────────────────┘
                            ▼
        ┌─────────────────────────────────────────┐
        │      C Scheduler (N worker threads)     │
        │  - Work-stealing between workers        │
        │  - pthread_cond_wait for parking        │
        │  - Lock-free queues where possible      │
        └─────────────────────────────────────────┘
```

---

## Phase 1: C Extension - Core Fiber Engine

### 1.1 Unified Fiber Structure

**Key Concept:** Both sync and async functions run on the same Fiber type.

**File:** `gsyncio/_gsyncio_core.c`

```c
#ifndef _GSYNCIO_CORE_H
#define _GSYNCIO_CORE_H

#include <Python.h>
#include <pthread.h>
#include <stdint.h>

// Fiber types - unified for both models
typedef enum {
    FIBER_SYNC,      // Sync function (PyCFunction wrapper)
    FIBER_ASYNC      // Async coroutine
} FiberType;

// Fiber states
typedef enum {
    FIBER_STATE_NEW,
    FIBER_STATE_RUNNING,
    FIBER_STATE_SUSPENDED,
    FIBER_STATE_DONE
} FiberState;

// Fiber object - unified for both sync and async
typedef struct Fiber {
    PyObject_HEAD
    
    // Common fields
    FiberType type;              // SYNC or ASYNC
    FiberState state;            // Current state
    int id;                      // Unique fiber ID
    int pending;                 // 1 if in pending queue
    
    // For async fibers
    PyObject *coro;              // Python coroutine (ASYNC only)
    PyObject *awaited;           // Current awaitable (ASYNC only)
    
    // For sync fibers
    PyCFunction func;            // C function (SYNC only)
    PyObject *args;              // Function args (SYNC only)
    PyObject *result;            // Return value (SYNC only)
    
    // Thread state for GIL management
    PyThreadState *tstate;
    
    // Linked list pointers for queues
    struct Fiber *next;
    struct Fiber *prev;
} FiberObject;
```

### 1.2 Fiber Creation - Both Sync and Async

```c
// Create async fiber from coroutine
static FiberObject* fiber_create_async(PyObject *coro) {
    if (!PyCoro_CheckExact(coro)) {
        PyErr_SetString(PyExc_TypeError, "Expected coroutine");
        return NULL;
    }
    
    FiberObject *f = PyObject_New(FiberObject, &FiberType);
    f->type = FIBER_ASYNC;
    f->state = FIBER_STATE_NEW;
    f->coro = coro;
    f->awaited = NULL;
    f->func = NULL;
    f->args = NULL;
    f->result = NULL;
    f->id = atomic_fetch_add(&_fiber_id_counter, 1);
    
    Py_INCREF(coro);
    return f;
}

// Create sync fiber from function + args
static FiberObject* fiber_create_sync(PyCFunction func, PyObject *args) {
    FiberObject *f = PyObject_New(FiberObject, &FiberType);
    f->type = FIBER_SYNC;
    f->state = FIBER_STATE_NEW;
    f->coro = NULL;
    f->awaited = NULL;
    f->func = func;
    f->args = args;
    f->result = NULL;
    f->id = atomic_fetch_add(&_fiber_id_counter, 1);
    
    Py_INCREF(args);
    return f;
}
```

### 1.3 Unified Fiber Resume

```c
// Resume fiber execution - handles both sync and async
static void fiber_resume(FiberObject *f) {
    if (!f || f->state == FIBER_STATE_DONE) return;
    
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    if (f->type == FIBER_ASYNC) {
        // === ASYNC: Resume coroutine ===
        PyObject *result = NULL;
        
        if (f->awaited == NULL) {
            result = PyObject_Send(f->coro, Py_None);
        } else {
            result = PyObject_Send(f->coro, f->awaited);
            Py_DECREF(f->awaited);
            f->awaited = NULL;
        }
        
        if (result == NULL) {
            if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
                PyErr_Clear();
                f->state = FIBER_STATE_DONE;
                Py_DECREF(f->coro);
                f->coro = NULL;
            }
            PyGILState_Release(gstate);
            return;
        }
        
        if (PyObject_TypeCheck(result, &FutureType)) {
            // Yielded a Future - wait for it
            f->awaited = result;
            f->state = FIBER_STATE_SUSPENDED;
            ((FutureObject*)result)->fiber = f;
            loop_add_pending(_current_loop, f);
        } else {
            Py_DECREF(result);
            loop_add_ready(_current_loop, f);
        }
        
    } else {
        // === SYNC: Call function directly ===
        if (f->func != NULL) {
            f->result = f->func(f->args);
            f->state = FIBER_STATE_DONE;
            Py_DECREF(f->args);
            f->args = NULL;
            // Sync function completes immediately
        }
    }
    
    PyGILState_Release(gstate);
}
```

// Future object (C implementation)
typedef struct Future {
    PyObject_HEAD
    
    struct Fiber *fiber;         // Fiber waiting on this future
    PyObject *result;            // Result value
    PyObject *exception;         // Exception if failed
    PyObject *callbacks;         // List of callbacks
    int done;                    // Completion flag
} FutureObject;

// Timer for sleep/wakeup
typedef struct Timer {
    int64_t deadline_ns;         // Nanoseconds since epoch
    FiberObject *fiber;          // Fiber to wake
    struct Timer *next;
} TimerObject;

// Event loop (per-thread)
typedef struct EventLoop {
    PyObject_HEAD
    
    FiberObject *ready_queue;    // Fibers ready to run
    FiberObject *pending_queue;  // Fibers waiting on awaitables
    FiberObject *main_fiber;     // Main coroutine fiber
    
    TimerObject *timer_queue;    // Scheduled timers
    
    pthread_mutex_t lock;        // Queue protection
    pthread_cond_t cond;         // Wait condition
    
    int running;                 // Loop is running
    int fiber_count;             // Active fiber count
    int pending_count;           // Pending fiber count
} EventLoopObject;

// Thread-local current loop
static __thread EventLoopObject *_current_loop = NULL;

// Global fiber ID counter
static atomic_int _fiber_id_counter = 0;

#endif // _GSYNCIO_CORE_H
```

---

### 1.2 Fiber Operations

**File:** `gsyncio/_gsyncio_core.c`

```c
// Create a new fiber from a coroutine
static FiberObject* fiber_create(PyObject *coro) {
    if (!PyCoro_CheckExact(coro)) {
        PyErr_SetString(PyExc_TypeError, "Expected coroutine");
        return NULL;
    }
    
    FiberObject *f = PyObject_New(FiberObject, &FiberType);
    if (!f) return NULL;
    
    f->coro = coro;
    f->awaited = NULL;
    f->tstate = PyGILState_GetThisThreadState();
    f->state = FIBER_STATE_NEW;
    f->id = atomic_fetch_add(&_fiber_id_counter, 1);
    f->pending = 0;
    f->next = NULL;
    f->prev = NULL;
    
    Py_INCREF(coro);
    return f;
}

// Resume fiber execution
static void fiber_resume(FiberObject *f) {
    if (!f || f->state == FIBER_STATE_DONE) return;
    
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    PyObject *result = NULL;
    
    // Send value to coroutine (None for first resume, or future result)
    if (f->awaited == NULL) {
        result = PyObject_Send(f->coro, Py_None);
    } else {
        result = PyObject_Send(f->coro, f->awaited);
        Py_DECREF(f->awaited);
        f->awaited = NULL;
    }
    
    if (result == NULL) {
        // Exception or StopAsyncIteration
        if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
            PyErr_Clear();
            f->state = FIBER_STATE_DONE;
            Py_DECREF(f->coro);
            f->coro = NULL;
        } else {
            // Store exception for later retrieval
            f->state = FIBER_STATE_DONE;
            // Fetch and store exception...
        }
        PyGILState_Release(gstate);
        return;
    }
    
    if (result == Py_None) {
        // Coroutine yielded None - schedule next step
        Py_DECREF(result);
        loop_add_ready(_current_loop, f);
    } else if (PyObject_TypeCheck(result, &FutureType)) {
        // Coroutine yielded a Future - wait for it
        f->awaited = result;
        f->state = FIBER_STATE_SUSPENDED;
        FutureObject *fut = (FutureObject*)result;
        fut->fiber = f;  // Link fiber to future
        loop_add_pending(_current_loop, f);
    } else {
        // Unknown awaitable - treat as done
        Py_DECREF(result);
        loop_add_ready(_current_loop, f);
    }
    
    PyGILState_Release(gstate);
}

// Park current fiber (called from Python)
static PyObject* fiber_park_py(PyObject *self, PyObject *args) {
    if (!_current_loop) {
        PyErr_SetString(PyExc_RuntimeError, "No event loop running");
        return NULL;
    }
    
    // Get current fiber from thread-local storage
    FiberObject *current = get_current_fiber();
    if (!current) {
        PyErr_SetString(PyExc_RuntimeError, "No current fiber");
        return NULL;
    }
    
    // Move to pending queue and wait
    current->state = FIBER_STATE_SUSPENDED;
    loop_add_pending(_current_loop, current);
    
    // Release GIL and wait on condition
    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&_current_loop->lock);
    while (current->state == FIBER_STATE_SUSPENDED) {
        pthread_cond_wait(&_current_loop->cond, &_current_loop->lock);
    }
    pthread_mutex_unlock(&_current_loop->lock);
    Py_END_ALLOW_THREADS
    
    Py_RETURN_NONE;
}

// Unpark a fiber (move to ready queue)
static void fiber_unpark(FiberObject *f) {
    if (!f || f->state != FIBER_STATE_SUSPENDED) return;
    
    pthread_mutex_lock(&_current_loop->lock);
    f->state = FIBER_STATE_RUNNING;
    loop_add_ready(_current_loop, f);
    pthread_cond_signal(&_current_loop->cond);
    pthread_mutex_unlock(&_current_loop->lock);
}
```

---

### 1.3 Event Loop Operations

```c
// Create event loop
static EventLoopObject* loop_create(void) {
    EventLoopObject *loop = PyObject_New(EventLoopObject, &EventLoopType);
    if (!loop) return NULL;
    
    loop->ready_queue = NULL;
    loop->pending_queue = NULL;
    loop->main_fiber = NULL;
    loop->timer_queue = NULL;
    loop->running = 0;
    loop->fiber_count = 0;
    loop->pending_count = 0;
    
    pthread_mutex_init(&loop->lock, NULL);
    pthread_cond_init(&loop->cond, NULL);
    
    return loop;
}

// Add fiber to ready queue
static void loop_add_ready(EventLoopObject *loop, FiberObject *f) {
    pthread_mutex_lock(&loop->lock);
    f->next = loop->ready_queue;
    if (loop->ready_queue) {
        loop->ready_queue->prev = f;
    }
    loop->ready_queue = f;
    f->state = FIBER_STATE_RUNNING;
    pthread_cond_signal(&loop->cond);
    pthread_mutex_unlock(&loop->lock);
}

// Add fiber to pending queue
static void loop_add_pending(EventLoopObject *loop, FiberObject *f) {
    pthread_mutex_lock(&loop->lock);
    f->next = loop->pending_queue;
    if (loop->pending_queue) {
        loop->pending_queue->prev = f;
    }
    loop->pending_queue = f;
    loop->pending_count++;
    pthread_mutex_unlock(&loop->lock);
}

// Run event loop until all fibers complete
static void loop_run(EventLoopObject *loop) {
    _current_loop = loop;
    loop->running = 1;
    
    while (loop->running) {
        // Process ready fibers
        pthread_mutex_lock(&loop->lock);
        while (loop->ready_queue) {
            FiberObject *f = loop->ready_queue;
            loop->ready_queue = f->next;
            pthread_mutex_unlock(&loop->lock);
            
            fiber_resume(f);
            
            pthread_mutex_lock(&loop->lock);
        }
        
        // Check timers
        timer_check(loop);
        
        // Check if done
        if (loop->main_fiber && loop->main_fiber->state == FIBER_STATE_DONE) {
            loop->running = 0;
        }
        
        // Wait for work if nothing ready
        if (loop->running && !loop->ready_queue) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 1000000;  // 1ms timeout
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&loop->cond, &loop->lock, &ts);
        }
        
        pthread_mutex_unlock(&loop->lock);
    }
    
    _current_loop = NULL;
}
```

---

### 1.4 Future Operations

```c
// Create future
static FutureObject* future_create(EventLoopObject *loop) {
    FutureObject *fut = PyObject_New(FutureObject, &FutureType);
    if (!fut) return NULL;
    
    fut->fiber = NULL;
    fut->result = NULL;
    fut->exception = NULL;
    fut->callbacks = PyList_New(0);
    fut->done = 0;
    
    return fut;
}

// Set future result and unpark waiting fiber
static void future_set_result(FutureObject *fut, PyObject *result) {
    if (fut->done) {
        PyErr_SetString(PyExc_RuntimeError, "Future already completed");
        return;
    }
    
    fut->result = result;
    fut->done = 1;
    Py_XINCREF(result);
    
    // Call callbacks
    if (fut->callbacks) {
        PyObject *args = PyTuple_Pack(1, fut);
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(fut->callbacks); i++) {
            PyObject *cb = PyList_GET_ITEM(fut->callbacks, i);
            PyObject_CallObject(cb, args);
            PyErr_Clear();  // Ignore callback errors
        }
        Py_DECREF(args);
    }
    
    // Unpark waiting fiber
    if (fut->fiber) {
        fiber_unpark(fut->fiber);
        fut->fiber = NULL;
    }
}

// Set future exception
static void future_set_exception(FutureObject *fut, PyObject *exc) {
    if (fut->done) {
        PyErr_SetString(PyExc_RuntimeError, "Future already completed");
        return;
    }
    
    fut->exception = exc;
    fut->done = 1;
    Py_XINCREF(exc);
    
    // Call callbacks
    if (fut->callbacks) {
        PyObject *args = PyTuple_Pack(1, fut);
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(fut->callbacks); i++) {
            PyObject *cb = PyList_GET_ITEM(fut->callbacks, i);
            PyObject_CallObject(cb, args);
            PyErr_Clear();
        }
        Py_DECREF(args);
    }
    
    // Unpark waiting fiber
    if (fut->fiber) {
        fiber_unpark(fut->fiber);
        fut->fiber = NULL;
    }
}
```

---

### 1.5 Timer Operations

```c
// Get current time in nanoseconds
static int64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Schedule timer to wake fiber
static void timer_schedule(int64_t delay_ns, FiberObject *fiber) {
    TimerObject *timer = malloc(sizeof(TimerObject));
    timer->deadline_ns = get_time_ns() + delay_ns;
    timer->fiber = fiber;
    
    // Insert into sorted timer queue (by deadline)
    pthread_mutex_lock(&_current_loop->lock);
    TimerObject **pp = &_current_loop->timer_queue;
    while (*pp && (*pp)->deadline_ns < timer->deadline_ns) {
        pp = &(*pp)->next;
    }
    timer->next = *pp;
    *pp = timer;
    pthread_mutex_unlock(&_current_loop->lock);
}

// Check and process expired timers
static void timer_check(EventLoopObject *loop) {
    int64_t now = get_time_ns();
    
    pthread_mutex_lock(&loop->lock);
    while (loop->timer_queue && loop->timer_queue->deadline_ns <= now) {
        TimerObject *timer = loop->timer_queue;
        loop->timer_queue = timer->next;
        fiber_unpark(timer->fiber);
        free(timer);
    }
    pthread_mutex_unlock(&loop->lock);
}
```

---

### 1.6 Python Module Definition

```c
// Module methods
static PyMethodDef module_methods[] = {
    {"fiber_create", (PyCFunction)fiber_create_py, METH_O, 
     "Create a fiber from a coroutine"},
    {"fiber_resume", (PyCFunction)fiber_resume_py, METH_O, 
     "Resume a fiber"},
    {"fiber_park", (PyCFunction)fiber_park_py, METH_NOARGS, 
     "Park current fiber"},
    {"loop_create", (PyCFunction)loop_create_py, METH_NOARGS, 
     "Create an event loop"},
    {"loop_run", (PyCFunction)loop_run_py, METH_O, 
     "Run event loop"},
    {"future_create", (PyCFunction)future_create_py, METH_O, 
     "Create a future"},
    {"future_set_result", (PyCFunction)future_set_result_py, METH_VARARGS, 
     "Set future result"},
    {"future_set_exception", (PyCFunction)future_set_exception_py, METH_VARARGS, 
     "Set future exception"},
    {"schedule_wakeup", (PyCFunction)schedule_wakeup_py, METH_VARARGS, 
     "Schedule fiber wakeup after delay"},
    {"current_fiber_id", (PyCFunction)current_fiber_id_py, METH_NOARGS, 
     "Get current fiber ID"},
    {NULL, NULL, 0, NULL}
};

// Module initialization
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_gsyncio_core",
    "Native C fiber scheduler for gsyncio",
    -1,
    module_methods
};

PyMODINIT_FUNC PyInit__gsyncio_core(void) {
    PyObject *m;
    
    if (PyType_Ready(&FiberType) < 0) return NULL;
    if (PyType_Ready(&FutureType) < 0) return NULL;
    if (PyType_Ready(&EventLoopType) < 0) return NULL;
    
    m = PyModule_Create(&moduledef);
    if (!m) return NULL;
    
    Py_INCREF(&FiberType);
    PyModule_AddObject(m, "Fiber", (PyObject*)&FiberType);
    Py_INCREF(&FutureType);
    PyModule_AddObject(m, "Future", (PyObject*)&FutureType);
    Py_INCREF(&EventLoopType);
    PyModule_AddObject(m, "EventLoop", (PyObject*)&EventLoopType);
    
    return m;
}
```

---

## Phase 2: Python Wrappers

### 2.1 Core Module (`gsyncio/core.py`)

```python
"""
gsyncio.core - Python wrappers for native C fiber scheduler
"""

from ._gsyncio_core import (
    fiber_create as _fiber_create,
    fiber_resume as _fiber_resume,
    fiber_park as _fiber_park,
    loop_create as _loop_create,
    loop_run as _loop_run,
    future_create as _future_create,
    future_set_result as _future_set_result,
    future_set_exception as _future_set_exception,
    schedule_wakeup as _schedule_wakeup,
    current_fiber_id as _current_fiber_id,
)

# Thread-local storage for current loop
import threading
_local = threading.local()


def get_current_loop():
    """Get current thread's event loop."""
    return getattr(_local, 'loop', None)


def set_current_loop(loop):
    """Set current thread's event loop."""
    _local.loop = loop


class Fiber:
    """Python wrapper for C fiber."""
    
    def __init__(self, coro):
        self._fiber = _fiber_create(coro)
    
    def resume(self):
        """Resume fiber execution."""
        _fiber_resume(self._fiber)


class EventLoop:
    """Python wrapper for C event loop."""
    
    def __init__(self):
        self._loop = _loop_create()
    
    def run(self):
        """Run event loop until completion."""
        set_current_loop(self)
        try:
            _loop_run(self._loop)
        finally:
            set_current_loop(None)
    
    def create_future(self):
        """Create a future."""
        return Future(self._loop)
    
    def schedule_wakeup(self, delay_ns, callback):
        """Schedule wakeup after delay."""
        _schedule_wakeup(self._loop, delay_ns, callback)


class Future:
    """Python wrapper for C future."""
    
    def __init__(self, loop):
        self._future = _future_create(loop)
        self._callbacks = []
    
    def __await__(self):
        """Await the future - parks current fiber."""
        if not self.done():
            _fiber_park()
        if self._future.exception:
            raise self._future.exception
        return self._future.result
    
    def set_result(self, result):
        """Set future result."""
        _future_set_result(self._future, result)
    
    def set_exception(self, exc):
        """Set future exception."""
        _future_set_exception(self._future, exc)
    
    def add_done_callback(self, callback):
        """Add callback for when future completes."""
        if self.done():
            callback(self)
        else:
            self._callbacks.append(callback)
    
    def done(self):
        """Check if future is done."""
        return self._future.done
    
    def result(self):
        """Get future result."""
        if self._future.exception:
            raise self._future.exception
        return self._future.result


def current_fiber_id():
    """Get current fiber ID."""
    return _current_fiber_id()


def fiber_park():
    """Park current fiber."""
    _fiber_park()
```

---

## Phase 3: Channel Implementation

### 3.1 Channel (`gsyncio/channel.py`)

```python
"""
gsyncio.channel - Channel-based communication with native fibers
"""

from typing import Any, Optional, Generic, TypeVar
from .core import Channel as _Channel, Future, get_current_loop, fiber_park

T = TypeVar('T')


class Chan(Generic[T]):
    """Channel supporting both sync and async operations."""
    
    def __init__(self, capacity: int = 0):
        self._channel = _Channel(capacity)
        self._waiters_send = []
        self._waiters_recv = []
    
    # === Sync API (for gs.task sync functions) ===
    
    def send(self, value: T) -> None:
        """Blocking send for sync functions."""
        while not self._channel.send_nowait(value):
            if self._channel.closed:
                raise RuntimeError("Channel closed")
            fiber_park()  # Park until space available
    
    def recv(self) -> Optional[T]:
        """Blocking recv for sync functions."""
        while True:
            val = self._channel.recv_nowait()
            if val is not None:
                return val
            if self._channel.closed:
                return None
            fiber_park()  # Park until data available
    
    # === Async API (for async functions) ===
    
    async def send_async(self, value: T) -> None:
        """Async send with fiber parking."""
        if not self._channel.send_nowait(value):
            if self._channel.closed:
                raise RuntimeError("Channel closed")
            fut = get_current_loop().create_future()
            self._waiters_send.append(fut)
            await fut  # Parks fiber
            if not self._channel.send_nowait(value):
                raise RuntimeError("Channel closed")
    
    async def recv_async(self) -> T:
        """Async recv with fiber parking."""
        val = self._channel.recv_nowait()
        if val is not None:
            return val
        if self._channel.closed:
            raise StopAsyncIteration("Channel closed")
        fut = get_current_loop().create_future()
        self._waiters_recv.append(fut)
        await fut  # Parks fiber
        val = self._channel.recv_nowait()
        if val is None and self._channel.closed:
            raise StopAsyncIteration("Channel closed")
        return val
    
    # === Iterators ===
    
    def __iter__(self):
        """Sync iterator."""
        while True:
            val = self.recv()
            if val is None:
                return
            yield val
    
    def __aiter__(self):
        """Async iterator."""
        return self
    
    async def __anext__(self) -> T:
        try:
            return await self.recv_async()
        except StopAsyncIteration:
            raise
    
    # === Channel management ===
    
    def close(self) -> None:
        """Close the channel."""
        self._channel.close()
        # Wake all waiters
        for fut in self._waiters_send + self._waiters_recv:
            if not fut.done():
                fut.set_result(None)
        self._waiters_send.clear()
        self._waiters_recv.clear()
    
    @property
    def closed(self) -> bool:
        """Check if channel is closed."""
        return self._channel.closed
    
    @property
    def size(self) -> int:
        """Current buffer size."""
        return self._channel.size


def create_chan(capacity: int = 0) -> Chan:
    """Create a new channel."""
    return Chan(capacity)


async def send(ch: Chan[T], value: T) -> None:
    """Send value to channel."""
    await ch.send_async(value)


async def recv(ch: Chan[T]) -> T:
    """Receive value from channel."""
    return await ch.recv_async()


def close(ch: Chan) -> None:
    """Close channel."""
    ch.close()


__all__ = ['Chan', 'create_chan', 'send', 'recv', 'close']
```

---

## Phase 4: Task/Sync and Entry Point

### 4.1 Task Module (`gsyncio/task.py`)

```python
"""
gsyncio.task - Task/Sync model with native fiber support
"""

import inspect
from typing import Callable, Any
from .core import (
    EventLoop, 
    Fiber, 
    get_current_loop, 
    set_current_loop,
    init_scheduler,
    shutdown_scheduler,
    num_workers,
    atomic_task_count,
    atomic_all_tasks_complete,
)
from .channel import Chan


def run(func: Callable, *args, **kwargs) -> Any:
    """
    Run sync or async function as main entry point.
    
    Usage:
        # Async entry point
        async def main():
            await gs.sleep(1000)
        gs.run(main())
        
        # Sync entry point
        def main():
            print("Hello")
        gs.run(main)
    """
    # Check if func is a coroutine (already called async function)
    if inspect.iscoroutine(func):
        coro = func
    elif callable(func):
        result = func(*args, **kwargs)
        if inspect.iscoroutine(result):
            coro = result
        else:
            return result  # Sync function, just return
    else:
        raise TypeError(f"Expected callable or coroutine, got {type(func)}")
    
    # Create native C event loop
    loop = EventLoop()
    set_current_loop(loop)
    
    # Initialize C scheduler
    init_scheduler(num_workers=num_workers())
    
    try:
        # Create main fiber from coroutine
        main_fiber = Fiber(coro)
        loop._loop.main_fiber = main_fiber._fiber
        
        # Run the loop
        return loop.run()
    finally:
        # Cleanup
        shutdown_scheduler(wait=True)
        set_current_loop(None)


def task(func: Callable, *args, **kwargs):
    """
    Spawn a task (sync or async).
    
    For sync functions: executes immediately in current context
    For async functions: schedules on event loop
    """
    result = func(*args, **kwargs)
    
    if inspect.iscoroutine(result):
        # Async function - schedule on current event loop
        loop = get_current_loop()
        if loop:
            loop.create_task(result)
        else:
            raise RuntimeError("No event loop for async task")
    
    # Sync function already executed
    return True


def sync():
    """Wait for all tasks to complete."""
    while not atomic_all_tasks_complete():
        # Run pending async tasks while waiting
        loop = get_current_loop()
        if loop:
            loop._run_ready()
        # Small yield to avoid busy-wait
        from .core import yield_execution
        yield_execution()


def task_count() -> int:
    """Get number of active tasks."""
    return atomic_task_count()


__all__ = ['run', 'task', 'sync', 'task_count']
```

### 4.2 Execution Flow - Both Models Together

```
gs.run(main())  # main() is async
    │
    ├─> Create EventLoop (C)
    ├─> Create main_fiber (async type)
    └─> loop.run()
            │
            ├─> Resume main_fiber (async)
            │       │
            │       ├─> main() coroutine starts
            │       │
            │       ├─> gs.task(sync_worker, 1)
            │       │       ├─> sync_worker(1) called immediately
            │       │       └─> Returns (fire-and-forget)
            │       │
            │       ├─> gs.task(async_worker, 2)
            │       │       ├─> async_worker(2) returns coroutine
            │       │       └─> Creates async fiber, schedules on loop
            │       │
            │       ├─> gs.sync()
            │               └─> Parks main_fiber until all done
            │
            ├─> Resume async_worker fiber
            │       ├─> await gs.sleep(100)
            │       │       └─> Parks fiber, adds to timer queue
            │       └─> Fiber suspended
            │
            ├─> Timer expires (100ms)
            │       └─> Unparks async_worker fiber
            │
            ├─> Resume async_worker fiber
            │       └─> Completes, fiber done
            │
            └─> All tasks done, main_fiber unparked
                    └─> loop.run() returns
```

### 4.3 Usage Examples

#### Example 1: Pure Sync (Existing Model)
```python
import gsyncio as gs

def worker(n):
    print(f"Worker {n}")

def main():
    for i in range(5):
        gs.task(worker, i)
    gs.sync()

gs.run(main)  # Sync entry point
```

#### Example 2: Pure Async (New Model)
```python
import gsyncio as gs

async def worker(n):
    await gs.sleep(100)
    print(f"Worker {n}")

async def main():
    for i in range(5):
        gs.task(worker, i)
    gs.sync()

gs.run(main())  # Async entry point
```

#### Example 3: Mixed Model (Both Together)
```python
import gsyncio as gs

# Sync worker - fire-and-forget
def sync_worker(n):
    print(f"Sync worker {n}: {sum(range(1000))}")

# Async worker - uses await
async def async_worker(n):
    await gs.sleep(50)
    print(f"Async worker {n}")

async def main():
    # Spawn sync tasks (execute immediately)
    for i in range(3):
        gs.task(sync_worker, i)
    
    # Spawn async tasks (scheduled on loop)
    for i in range(3):
        gs.task(async_worker, i)
    
    # Wait for async tasks
    # (sync tasks already done)
    gs.sync()
    print("All complete")

gs.run(main())
```

#### Example 4: Channel Communication (Both Models)
```python
import gsyncio as gs

# Sync producer
def sync_producer(ch):
    for i in range(5):
        ch.send(i)  # Blocking send
    gs.close(ch)

# Async consumer
async def async_consumer(ch):
    async for val in ch:
        print(f"Received: {val}")

async def main():
    ch = gs.create_chan()
    gs.task(sync_producer, ch)  # Sync task
    await async_consumer(ch)     # Async in main

gs.run(main())
```

---

## Phase 5: Async Primitives

### 5.1 Sleep and Gather (`gsyncio/async_.py`)

```python
"""
gsyncio.async_ - Async primitives with native fibers
"""

from .core import get_current_loop, Future


async def sleep(ms: int) -> None:
    """
    Sleep for milliseconds without blocking thread.
    
    Uses native C timer to schedule fiber wakeup.
    """
    loop = get_current_loop()
    fut = loop.create_future()
    
    def wake():
        if not fut.done():
            fut.set_result(None)
    
    # Schedule C timer (convert ms to nanoseconds)
    delay_ns = ms * 1_000_000
    loop.schedule_wakeup(delay_ns, wake)
    
    # Park fiber until wakeup
    await fut


async def gather(*futures, return_exceptions: bool = False):
    """
    Wait for multiple futures to complete.
    """
    if not futures:
        return []
    
    results = [None] * len(futures)
    exceptions = [None] * len(futures)
    done_count = [0]
    
    def make_callback(i):
        def callback(fut):
            try:
                results[i] = fut.result()
            except Exception as e:
                exceptions[i] = e
            done_count[0] += 1
        return callback
    
    # Add callbacks to all futures
    for i, fut in enumerate(futures):
        if hasattr(fut, 'add_done_callback'):
            fut.add_done_callback(make_callback(i))
    
    # Wait for all to complete
    while done_count[0] < len(futures):
        await sleep(1)  # Yield while waiting
    
    # Build result list
    output = []
    for i, (result, exc) in enumerate(zip(results, exceptions)):
        if exc:
            if return_exceptions:
                output.append(exc)
            else:
                raise exc
        else:
            output.append(result)
    
    return output


async def wait_for(fut, timeout: float):
    """
    Wait for future with timeout.
    """
    import time
    
    deadline = time.time() + timeout
    
    while not fut.done():
        remaining = deadline - time.time()
        if remaining <= 0:
            raise TimeoutError("Wait timeout")
        await sleep(int(remaining * 1000))
    
    return fut.result()


__all__ = ['sleep', 'gather', 'wait_for']
```

---

## Phase 6: Module Exports

### 6.1 `gsyncio/__init__.py`

```python
"""
gsyncio - Native M:N Fiber Scheduler for Python

High-performance concurrency with:
- M:N greenlet scheduling (M fibers on N threads)
- Native C event loop (no asyncio dependency)
- Task/Sync model for fire-and-forget parallelism
- Async/Await model with fiber parking
- Channels for message-passing
"""

__version__ = '0.2.0'
__author__ = 'gsyncio team'

# Core
from .core import (
    Fiber,
    EventLoop,
    Future,
    current_fiber_id,
    fiber_park,
)

# Task/Sync
from .task import (
    run,
    task,
    sync,
    task_count,
)

# Channels
from .channel import (
    Chan,
    create_chan,
    send,
    recv,
    close,
)

# Async primitives
from .async_ import (
    sleep,
    gather,
    wait_for,
)

# C extension info
from ._gsyncio_core import (
    init_scheduler,
    shutdown_scheduler,
    num_workers,
    _HAS_NATIVE_SCHEDULER,
)

__all__ = [
    # Version
    '__version__',
    '__author__',
    
    # Core
    'Fiber',
    'EventLoop',
    'Future',
    'current_fiber_id',
    'fiber_park',
    
    # Task/Sync
    'run',
    'task',
    'sync',
    'task_count',
    
    # Channels
    'Chan',
    'create_chan',
    'send',
    'recv',
    'close',
    
    # Async primitives
    'sleep',
    'gather',
    'wait_for',
    
    # Scheduler
    'init_scheduler',
    'shutdown_scheduler',
    'num_workers',
    '_HAS_NATIVE_SCHEDULER',
]
```

---

## File Structure

```
gsyncio/
├── __init__.py              # Module exports
├── core.py                  # Python C extension wrappers
├── _gsyncio_core.c          # Native C fiber scheduler (Phase 1-14)
├── _gsyncio_core.h          # C header file
├── task.py                  # run(), task(), sync()
├── channel.py               # Chan (sync + async API)
├── async_.py                # sleep(), gather(), wait_for()
├── io.py                    # Async I/O helpers (Phase 9)
├── primitives.py            # Lock, Event, Semaphore (Phase 10)
├── debug.py                 # Fiber introspection (Phase 11)
├── cancellation.py          # Cancellation tokens (Phase 8)
└── setup.py                 # Build C extension
```

## Performance Goals

| Operation | Target | Notes |
|-----------|--------|-------|
| Fiber create | < 1 µs | Pure C allocation |
| Fiber park/unpark | < 500 ns | pthread_cond_wait |
| Channel send/recv | < 2 µs | Lock-free where possible |
| sleep(0) yield | < 500 ns | Direct scheduler call |
| Task spawn | < 1 µs | C atomic increment |
| Thread pool submit | < 2 µs | Lock-free where possible |
| I/O poll (100 fds) | < 100 µs | epoll_wait batching |

---

## Usage Examples

### Async Entry Point
```python
import gsyncio as gs

async def get_data() -> str:
    await gs.sleep(1000)
    return "The requested data"

async def main():
    # Returns coroutine object
    coro = get_data()
    print(f"Coroutine: {coro}")
    
    # Await to get result
    result = await get_data()
    print(f"Result: {result}")

if __name__ == "__main__":
    gs.run(main())  # Pure C scheduler, no asyncio
```

### Sync Entry Point
```python
import gsyncio as gs

def worker(n):
    print(f"Worker {n}")

def main():
    gs.task(worker, 1)
    gs.task(worker, 2)
    gs.sync()

gs.run(main)
```

### Mixed Model
```python
import gsyncio as gs

def sync_worker(n):
    print(f"Sync worker {n}")

async def async_worker(n):
    await gs.sleep(100)
    print(f"Async worker {n}")

async def main():
    # Spawn sync task
    gs.task(sync_worker, 1)
    
    # Spawn async task
    gs.task(async_worker, 2)
    
    gs.sync()  # Wait for all

gs.run(main())
```

### Channel Communication
```python
import gsyncio as gs

async def producer(ch):
    for i in range(5):
        await gs.send(ch, i)
    gs.close(ch)

async def consumer(ch):
    async for val in ch:
        print(f"Received: {val}")

async def main():
    ch = gs.create_chan()
    gs.task(producer, ch)
    await consumer(ch)

gs.run(main())
```

---

## Build Configuration

### `setup.py`
```python
from setuptools import setup, Extension

module = Extension(
    'gsyncio._gsyncio_core',
    sources=['gsyncio/_gsyncio_core.c'],
    extra_compile_args=['-std=c11', '-O3'],
    libraries=['pthread'],
)

setup(
    name='gsyncio',
    version='0.2.0',
    ext_modules=[module],
    packages=['gsyncio'],
)
```

---

## Implementation Checklist

- [ ] **Phase 1: C Extension - Core**
  - [ ] Define Fiber, Future, EventLoop structs
  - [ ] Implement fiber_create, fiber_resume, fiber_park
  - [ ] Implement loop_create, loop_run, loop_add_ready
  - [ ] Implement future_create, future_set_result
  - [ ] Implement timer_schedule, timer_check
  - [ ] Create Python module definition
  - [ ] **FIX: Remove fibers from pending queue on resume (BUG FIX)**
  - [ ] **FIX: Add memory barriers for thread-safe queue operations**
  - [ ] **FIX: Proper fiber_cleanup() from all queues before free**
  - [ ] **FIX: timer_cancel_fiber() and timer_queue_clear() on loop close**

- [ ] **Phase 2: Python Wrappers**
  - [ ] Create Fiber, EventLoop, Future classes
  - [ ] Implement get_current_loop, set_current_loop
  - [ ] Test basic fiber creation and resume

- [ ] **Phase 3: Channel**
  - [ ] Implement Chan class with sync send/recv
  - [ ] Implement async send_async/recv_async
  - [ ] Add sync and async iterators
  - [ ] Test channel communication

- [ ] **Phase 4: Task/Sync**
  - [ ] Implement run() for async entry point
  - [ ] Implement task() for spawning
  - [ ] Implement sync() for waiting
  - [ ] Test task spawning and sync

- [ ] **Phase 5: Async Primitives**
  - [ ] Implement sleep() with C timers
  - [ ] Implement gather()
  - [ ] Implement wait_for()
  - [ ] Test async primitives

- [ ] **Phase 6: Integration**
  - [ ] Update __init__.py exports
  - [ ] Write documentation
  - [ ] Write tests
  - [ ] Benchmark performance

- [ ] **Phase 7: Thread Pool Management** (NEW)
  - [ ] Implement ThreadPoolObject struct with work-stealing queues
  - [ ] Implement worker_thread() main loop
  - [ ] Implement pool_submit(), pool_create(), pool_shutdown()
  - [ ] Implement init_scheduler_py(), shutdown_scheduler_py(), num_workers_py()
  - [ ] Implement graceful_shutdown() with three-phase protocol
  - [ ] Test thread pool with concurrent fibers

- [ ] **Phase 8: Cancellation Support** (NEW)
  - [ ] Implement CancelTokenObject with parent chain
  - [ ] Implement cancel_token_is_cancelled()
  - [ ] Implement fiber_cancel() and fiber_raise_cancelled()
  - [ ] Implement Python CancellationToken wrapper
  - [ ] Implement check_cancelled() async helper
  - [ ] Test cancellation propagation

- [ ] **Phase 9: Async I/O Integration** (NEW)
  - [ ] Implement IOWatcherObject with epoll/kqueue
  - [ ] Implement io_watcher_register(), io_watcher_poll()
  - [ ] Integrate I/O polling into main event loop
  - [ ] Implement Python sock_recv, sock_sendall, sock_accept
  - [ ] Test network I/O with fiber parking

- [ ] **Phase 10: Sync Primitives** (NEW)
  - [ ] Implement Lock with async context manager
  - [ ] Implement Event with wait/set/clear
  - [ ] Implement Semaphore with acquire/release
  - [ ] Implement Condition with wait/notify
  - [ ] Test concurrent access with primitives

- [ ] **Phase 11: Fiber Introspection** (NEW)
  - [ ] Implement FiberInfo struct and fiber_to_info()
  - [ ] Implement loop_get_fibers()
  - [ ] Implement Python get_all_fibers(), get_fiber_count()
  - [ ] Implement FiberDump context manager
  - [ ] Test fiber introspection in debug scenarios

- [ ] **Phase 12: Memory Management Fixes** (NEW - BUG FIXES)
  - [ ] Implement fiber_cleanup() to remove from all queues
  - [ ] Implement fiber_dealloc() with proper cleanup
  - [ ] Implement timer_cancel_fiber() for fiber cancellation
  - [ ] Implement timer_queue_clear() on loop destruction
  - [ ] Test memory leaks with long-running fibers

- [ ] **Phase 13: Exception Propagation** (NEW - BUG FIX)
  - [ ] Implement StoredException struct
  - [ ] Implement capture_exception(), restore_exception()
  - [ ] Update fiber_resume() to handle all exception paths
  - [ ] Test exception propagation in nested coroutines

- [ ] **Phase 14: Missing Python Helpers** (NEW - BUG FIX)
  - [ ] Implement atomic_task_inc(), atomic_task_dec()
  - [ ] Implement atomic_task_count(), atomic_all_tasks_complete()
  - [ ] Implement yield_execution_py()
  - [ ] Update Python wrappers to use C atomics
  - [ ] Test atomic counters under load

---

---

## Summary: Both Models Supported

### Task/Sync Model (Existing - Fire-and-Forget)

```python
import gsyncio as gs

def worker(n):
    print(f"Worker {n}")

def main():
    gs.task(worker, 1)  # Executes immediately
    gs.task(worker, 2)  # Executes immediately
    gs.sync()           # Wait for all

gs.run(main)  # Sync entry point (function, not call)
```

**Characteristics:**
- Sync functions execute immediately
- Fire-and-forget semantics
- No async/await keywords needed
- Best for CPU-bound or simple parallel tasks

### Async/Await Model (New - Native Fibers)

```python
import gsyncio as gs

async def worker(n):
    await gs.sleep(100)
    print(f"Worker {n}")

async def main():
    gs.task(worker, 1)  # Schedules on event loop
    gs.task(worker, 2)  # Schedules on event loop
    gs.sync()           # Wait for all

gs.run(main())  # Async entry point (coroutine)
```

**Characteristics:**
- Async functions scheduled on event loop
- Fiber parks on await (non-blocking)
- Uses async/await keywords
- Best for I/O-bound or concurrent operations

### Mixed Model (Both Together)

```python
import gsyncio as gs

def sync_worker(n):
    print(f"Sync: {n}")

async def async_worker(n):
    await gs.sleep(50)
    print(f"Async: {n}")

async def main():
    gs.task(sync_worker, 1)   # Sync - executes now
    gs.task(async_worker, 2)  # Async - schedules
    gs.sync()

gs.run(main())
```

**Key Points:**
- Both models share the same C fiber scheduler
- Sync tasks execute immediately (fire-and-forget)
- Async tasks are scheduled on the event loop
- Both use `gs.sync()` to wait for completion
- Both can communicate via channels

---

## Phase 7: Thread Pool Management

### 7.1 C Thread Pool Implementation

```c
// Thread pool state
typedef struct ThreadPool {
    pthread_t *workers;           // Worker threads
    int num_workers;               // Number of threads
    FiberObject *shared_queue;     // Work-stealing queue head
    FiberObject **shared_queue_tail;
    atomic_int active_workers;     // Currently running workers
    atomic_int shutdown_requested; // Shutdown flag
    
    pthread_mutex_t lock;          // Queue lock
    pthread_cond_t work_avail;     // New work notification
    pthread_cond_t idle;           // All workers idle
    
    // Per-worker local queues for work-stealing
    FiberObject **local_queues;    // Array of local queues
    int *local_queue_sizes;        // Array of local queue sizes
} ThreadPoolObject;

// Initialize thread pool
static ThreadPoolObject* pool_create(int num_workers) {
    ThreadPoolObject *pool = malloc(sizeof(ThreadPoolObject));
    if (!pool) return NULL;
    
    pool->num_workers = num_workers;
    pool->workers = malloc(num_workers * sizeof(pthread_t));
    pool->active_workers = num_workers;
    pool->shutdown_requested = 0;
    
    pool->shared_queue = NULL;
    pool->shared_queue_tail = &pool->shared_queue;
    
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->work_avail, NULL);
    pthread_cond_init(&pool->idle, NULL);
    
    // Per-worker local queues
    pool->local_queues = calloc(num_workers, sizeof(FiberObject*));
    pool->local_queue_sizes = calloc(num_workers, sizeof(int));
    
    // Start worker threads
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&pool->workers[i], NULL, worker_thread, 
                       (void*)(intptr_t)i);
    }
    
    return pool;
}

// Worker thread main loop
static void* worker_thread(void *arg) {
    int worker_id = (int)(intptr_t)arg;
    ThreadPoolObject *pool = _global_pool;
    
    while (!atomic_load(&pool->shutdown_requested)) {
        FiberObject *f = NULL;
        
        // Try local queue first (no lock needed)
        if (pool->local_queue_sizes[worker_id] > 0) {
            FiberObject **local = &pool->local_queues[worker_id];
            f = *local;
            *local = f->next;
            pool->local_queue_sizes[worker_id]--;
        }
        // Try stealing from other workers
        else {
            for (int i = 0; i < pool->num_workers; i++) {
                if (i == worker_id) continue;
                if (pool->local_queue_sizes[i] > 0) {
                    pthread_mutex_lock(&pool->lock);
                    FiberObject **local = &pool->local_queues[i];
                    if (*local) {
                        f = *local;
                        *local = f->next;
                        pool->local_queue_sizes[i]--;
                    }
                    pthread_mutex_unlock(&pool->lock);
                    break;
                }
            }
        }
        
        // Try shared queue
        if (!f) {
            pthread_mutex_lock(&pool->lock);
            if (pool->shared_queue) {
                f = pool->shared_queue;
                pool->shared_queue = f->next;
                if (pool->shared_queue == NULL) {
                    pool->shared_queue_tail = &pool->shared_queue;
                }
            }
            if (!f) {
                // Wait for work
                pthread_cond_wait(&pool->work_avail, &pool->lock);
            }
            pthread_mutex_unlock(&pool->lock);
        }
        
        // Execute fiber if we got one
        if (f) {
            fiber_resume(f);
        }
    }
    
    atomic_fetch_sub(&pool->active_workers, 1);
    return NULL;
}

// Submit fiber to thread pool
static void pool_submit(ThreadPoolObject *pool, FiberObject *f) {
    pthread_mutex_lock(&pool->lock);
    *pool->shared_queue_tail = f;
    pool->shared_queue_tail = &f->next;
    f->next = NULL;
    pthread_cond_signal(&pool->work_avail);
    pthread_mutex_unlock(&pool->lock);
}

// Shutdown thread pool
static void pool_shutdown(ThreadPoolObject *pool, int wait) {
    atomic_store(&pool->shutdown_requested, 1);
    
    // Wake all workers
    pthread_mutex_lock(&pool->lock);
    pthread_cond_broadcast(&pool->work_avail);
    pthread_mutex_unlock(&pool->lock);
    
    if (wait) {
        for (int i = 0; i < pool->num_workers; i++) {
            pthread_join(pool->workers[i], NULL);
        }
    }
    
    // Cleanup
    free(pool->workers);
    free(pool->local_queues);
    free(pool->local_queue_sizes);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->work_avail);
    pthread_cond_destroy(&pool->idle);
    free(pool);
}
```

### 7.2 Graceful Shutdown Protocol

```c
// Shutdown phases for graceful cleanup
typedef enum {
    SHUTDOWN_PHASE_1_DRAIN,    // Stop accepting new tasks
    SHUTDOWN_PHASE_2_CANCEL,   // Cancel pending fibers
    SHUTDOWN_PHASE_3_TERMINATE // Force terminate remaining
} ShutdownPhase;

// Graceful shutdown with timeout
static int graceful_shutdown(ThreadPoolObject *pool, int64_t timeout_ns) {
    int64_t deadline = get_time_ns() + timeout_ns;
    
    // Phase 1: Signal shutdown
    atomic_store(&pool->shutdown_requested, 1);
    
    // Phase 2: Wait for active fibers with timeout
    while (atomic_load(&pool->active_workers) > 0) {
        if (get_time_ns() >= deadline) {
            // Phase 3: Force terminate
            return -1;
        }
        usleep(1000);  // 1ms
    }
    
    return 0;
}
```

---

## Phase 8: Cancellation Support

### 8.1 Cancellation Tokens

```c
// Cancellation token object
typedef struct CancelToken {
    PyObject_HEAD
    atomic_int cancelled;        // Cancellation flag
    atomic_int shielded;         // Shield from parent cancellation
    struct CancelToken *parent;   // Parent token (weak ref)
} CancelTokenObject;

// Check if token is cancelled
static int cancel_token_is_cancelled(CancelTokenObject *token) {
    if (!token) return 0;
    
    if (atomic_load(&token->cancelled)) {
        return 1;
    }
    
    // Check parent chain
    if (token->parent && atomic_load(&token->parent->cancelled)) {
        // Only propagate if not shielded
        if (!atomic_load(&token->shielded)) {
            return 1;
        }
    }
    
    return 0;
}

// Throw cancellation exception in fiber
static int fiber_raise_cancelled(FiberObject *f) {
    if (!f || f->state == FIBER_STATE_DONE) return 0;
    
    PyGILState_STATE gstate = PyGILState_Ensure();
    PyErr_SetString(PyExc_OperationCancelledError, "Task cancelled");
    // Store exception for retrieval
    f->exception = PyErr_Occurred();
    Py_XINCREF(f->exception);
    PyGILState_Release(gstate);
    
    return 1;
}

// Cancel a fiber
static int fiber_cancel(FiberObject *f, CancelTokenObject *token) {
    if (!f || f->state == FIBER_STATE_DONE) return -1;
    
    if (token && !cancel_token_is_cancelled(token)) {
        return 0;  // Not cancelled yet
    }
    
    // If fiber is suspended, unpark with cancellation
    if (f->state == FIBER_STATE_SUSPENDED) {
        fiber_unpark(f);
    }
    
    // Mark as cancelled
    f->state = FIBER_STATE_CANCELLED;
    return 1;
}
```

### 8.2 Python Cancellation API

```python
class CancelledError(Exception):
    """Raised when operation is cancelled."""
    pass

class CancellationToken:
    """Token for cooperative cancellation."""
    
    def __init__(self):
        self._token = _CancelToken()
    
    @property
    def cancelled(self) -> bool:
        return self._token.cancelled
    
    def cancel(self):
        """Request cancellation."""
        self._token.cancel()
    
    def shield(self):
        """Create shielded child token."""
        child = CancellationToken()
        child._token.parent = self._token
        return child
    
    def throw(self, exc):
        """Throw exception in associated coroutines."""
        self._token.throw(exc)


async def check_cancelled(token: CancellationToken):
    """Check for cancellation, raise if cancelled."""
    if token.cancelled:
        raise CancelledError()
```

---

## Phase 9: Async I/O Integration

### 9.1 I/O Readiness via epoll/kqueue

```c
// I/O watcher for async I/O operations
typedef struct IOWatcher {
    int epoll_fd;                  // Linux: epoll, BSD: kqueue
    struct epoll_event *events;    // Event buffer
    int max_events;
    
    // Registered file descriptors
    struct {
        int fd;
        FiberObject *read_fiber;   // Fiber waiting for read
        FiberObject *write_fiber;  // Fiber waiting for write
        int events;                // epoll events mask
    } *watchers;
    int num_watchers;
    int capacity;
    
    pthread_mutex_t lock;
    pthread_cond_t io_ready;
} IOWatcherObject;

// Register file descriptor for I/O
static int io_watcher_register(IOWatcherObject *io, int fd, 
                                int events, FiberObject *fiber) {
    pthread_mutex_lock(&io->lock);
    
    // Expand capacity if needed
    if (io->num_watchers >= io->capacity) {
        io->capacity *= 2;
        io->watchers = realloc(io->watchers, 
                               io->capacity * sizeof(*io->watchers));
    }
    
    // Add to watchers array
    int idx = io->num_watchers++;
    io->watchers[idx].fd = fd;
    io->watchers[idx].events = events;
    io->watchers[idx].read_fiber = events & EPOLLIN ? fiber : NULL;
    io->watchers[idx].write_fiber = events & EPOLLOUT ? fiber : NULL;
    
    // Add to epoll
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(io->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    
    pthread_mutex_unlock(&io->lock);
    return 0;
}

// Process I/O events
static void io_watcher_poll(IOWatcherObject *io, int timeout_ms) {
    int n = epoll_wait(io->epoll_fd, io->events, io->max_events, timeout_ms);
    
    for (int i = 0; i < n; i++) {
        int fd = io->events[i].data.fd;
        uint32_t events = io->events[i].events;
        
        pthread_mutex_lock(&io->lock);
        
        // Find and unpark fiber
        for (int j = 0; j < io->num_watchers; j++) {
            if (io->watchers[j].fd != fd) continue;
            
            if (events & EPOLLIN && io->watchers[j].read_fiber) {
                fiber_unpark(io->watchers[j].read_fiber);
            }
            if (events & EPOLLOUT && io->watchers[j].write_fiber) {
                fiber_unpark(io->watchers[j].write_fiber);
            }
            if (events & (EPOLLERR | EPOLLHUP)) {
                // Error condition - unpark both
                fiber_unpark(io->watchers[j].read_fiber);
                fiber_unpark(io->watchers[j].write_fiber);
            }
            break;
        }
        
        pthread_mutex_unlock(&io->lock);
    }
}
```

### 9.2 Python Async I/O Helpers

```python
async def sock_recv(sock, n):
    """Receive from socket without blocking thread."""
    loop = get_current_loop()
    fut = loop.create_future()
    
    def on_readable():
        try:
            data = sock.recv(n)
            fut.set_result(data)
        except Exception as e:
            fut.set_exception(e)
    
    loop.add_reader(sock, on_readable)
    return await fut


async def sock_sendall(sock, data):
    """Send all data without blocking."""
    loop = get_current_loop()
    fut = loop.create_future()
    
    def on_writable():
        try:
            n = sock.send(data)
            if n == len(data):
                fut.set_result(n)
            else:
                data = data[n:]
        except Exception as e:
            fut.set_exception(e)
    
    loop.add_writer(sock, on_writable)
    return await fut


async def sock_accept(sock):
    """Accept connection without blocking."""
    loop = get_current_loop()
    fut = loop.create_future()
    
    def on_readable():
        try:
            conn, addr = sock.accept()
            fut.set_result((conn, addr))
        except Exception as e:
            fut.set_exception(e)
    
    loop.add_reader(sock, on_readable)
    return await fut
```

---

## Phase 10: Sync Primitives

### 10.1 Lock and Event

```python
class Lock:
    """Async lock for synchronizing access."""
    
    def __init__(self):
        self._locked = False
        self._waiters = []
    
    async def __aenter__(self):
        await self.acquire()
        return self
    
    async def __aexit__(self, *args):
        self.release()
    
    async def acquire(self):
        loop = get_current_loop()
        while self._locked:
            fut = loop.create_future()
            self._waiters.append(fut)
            await fut
        self._locked = True
    
    def release(self):
        self._locked = False
        if self._waiters:
            fut = self._waiters.pop(0)
            fut.set_result(None)


class Event:
    """Async event for signaling."""
    
    def __init__(self):
        self._set = False
        self._waiters = []
    
    async def wait(self):
        if self._set:
            return
        fut = get_current_loop().create_future()
        self._waiters.append(fut)
        await fut
    
    def set(self):
        self._set = True
        for fut in self._waiters:
            fut.set_result(None)
        self._waiters.clear()
    
    def is_set(self) -> bool:
        return self._set
    
    def clear(self):
        self._set = False


class Semaphore:
    """Async semaphore for limiting concurrency."""
    
    def __init__(self, value: int = 1):
        self._value = value
        self._waiters = []
    
    async def __aenter__(self):
        await self.acquire()
        return self
    
    async def __aexit__(self, *args):
        self.release()
    
    async def acquire(self):
        loop = get_current_loop()
        while self._value <= 0:
            fut = loop.create_future()
            self._waiters.append(fut)
            await fut
        self._value -= 1
    
    def release(self):
        self._value += 1
        if self._waiters:
            fut = self._waiters.pop(0)
            fut.set_result(None)


class Condition:
    """Async condition variable."""
    
    def __init__(self, lock=None):
        self._lock = lock or Lock()
        self._waiters = []
    
    async def __aenter__(self):
        await self._lock.acquire()
        return self
    
    async def __aexit__(self, *args):
        self._lock.release()
    
    async def wait(self):
        self.release()
        try:
            fut = get_current_loop().create_future()
            self._waiters.append(fut)
            await fut
        finally:
            await self._lock.acquire()
    
    def notify(self, n=1):
        for i in range(min(n, len(self._waiters))):
            fut = self._waiters.pop(0)
            fut.set_result(None)
    
    def notify_all(self):
        for fut in self._waiters:
            fut.set_result(None)
        self._waiters.clear()
```

---

## Phase 11: Fiber Introspection

### 11.1 Debug and Inspection API

```c
// Fiber info for introspection
typedef struct {
    int id;
    FiberType type;
    FiberState state;
    int pending;
    PyObject *awaited_obj;  // What fiber is waiting on
    PyObject *repr;         // String representation
} FiberInfo;

// Get all fibers in loop
static PyObject* loop_get_fibers(EventLoopObject *loop) {
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    PyObject *list = PyList_New(0);
    
    pthread_mutex_lock(&loop->lock);
    
    // Traverse ready queue
    FiberObject *f = loop->ready_queue;
    while (f) {
        PyObject *info = fiber_to_info(f);
        PyList_Append(list, info);
        Py_DECREF(info);
        f = f->next;
    }
    
    // Traverse pending queue
    f = loop->pending_queue;
    while (f) {
        PyObject *info = fiber_to_info(f);
        PyList_Append(list, info);
        Py_DECREF(info);
        f = f->next;
    }
    
    pthread_mutex_unlock(&loop->lock);
    PyGILState_Release(gstate);
    
    return list;
}

// Convert fiber to info dict
static PyObject* fiber_to_info(FiberObject *f) {
    PyObject *info = PyDict_New();
    PyDict_SetItemString(info, "id", PyLong_FromLong(f->id));
    
    const char *type_str = f->type == FIBER_SYNC ? "sync" : "async";
    PyDict_SetItemString(info, "type", PyUnicode_FromString(type_str));
    
    const char *state_str;
    switch (f->state) {
        case FIBER_STATE_NEW: state_str = "new"; break;
        case FIBER_STATE_RUNNING: state_str = "running"; break;
        case FIBER_STATE_SUSPENDED: state_str = "suspended"; break;
        case FIBER_STATE_DONE: state_str = "done"; break;
        case FIBER_STATE_CANCELLED: state_str = "cancelled"; break;
        default: state_str = "unknown";
    }
    PyDict_SetItemString(info, "state", PyUnicode_FromString(state_str));
    
    PyDict_SetItemString(info, "pending", PyBool_FromLong(f->pending));
    
    if (f->awaited) {
        Py_INCREF(f->awaited);
        PyDict_SetItemString(info, "awaited", f->awaited);
    }
    
    return info;
}
```

### 11.2 Python Debug API

```python
def get_all_fibers():
    """Get list of all fibers in current loop."""
    loop = get_current_loop()
    if not loop:
        return []
    return loop.get_fibers()


def get_fiber_count():
    """Get count of active fibers."""
    return len(get_all_fibers())


def current_fiber():
    """Get current fiber object."""
    return Fiber.current()


def fiber_stack_trace(fiber):
    """Get stack trace for a fiber (if suspended)."""
    # Uses PyFrameObject introspection
    pass


class FiberDump:
    """Context manager to dump fiber state on exception."""
    
    def __enter__(self):
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type:
            print("=== Fiber Dump ===")
            for info in get_all_fibers():
                print(f"  {info}")
        return False
```

---

## Phase 12: Memory Management Fixes

### 12.1 Proper Fiber Cleanup

```c
// Clear fiber from all queues before freeing
static void fiber_cleanup(FiberObject *f) {
    EventLoopObject *loop = _current_loop;
    if (!loop) return;
    
    pthread_mutex_lock(&loop->lock);
    
    // Remove from ready queue
    FiberObject **pp = &loop->ready_queue;
    while (*pp) {
        if (*pp == f) {
            *pp = f->next;
            if (f->next) {
                f->next->prev = *pp ? (*pp)->prev : NULL;
            }
            break;
        }
        pp = &(*pp)->next;
    }
    
    // Remove from pending queue
    pp = &loop->pending_queue;
    while (*pp) {
        if (*pp == f) {
            *pp = f->next;
            if (f->next) {
                f->next->prev = *pp ? (*pp)->prev : NULL;
            }
            loop->pending_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    
    pthread_mutex_unlock(&loop->lock);
    
    // Decrement active fiber count
    atomic_fetch_sub(&loop->fiber_count, 1);
}

// Deallocate fiber
static void fiber_dealloc(FiberObject *f) {
    // Untrack from GC
    PyObject_GC_UnTrack(f);
    
    // Cleanup from queues
    fiber_cleanup(f);
    
    // Clear references
    Py_CLEAR(f->coro);
    Py_CLEAR(f->awaited);
    Py_CLEAR(f->args);
    Py_CLEAR(f->result);
    Py_CLEAR(f->exception);
    
    // Free type-specific memory
    PyObject_Del(f);
}
```

### 12.2 Timer Memory Safety

```c
// Cancel and free all timers for a fiber
static void timer_cancel_fiber(EventLoopObject *loop, FiberObject *fiber) {
    pthread_mutex_lock(&loop->lock);
    
    TimerObject **pp = &loop->timer_queue;
    while (*pp) {
        if ((*pp)->fiber == fiber) {
            TimerObject *t = *pp;
            *pp = t->next;
            free(t);  // Timer was malloc'd
        } else {
            pp = &(*pp)->next;
        }
    }
    
    pthread_mutex_unlock(&loop->lock);
}

// Clear timer queue on loop close
static void timer_queue_clear(EventLoopObject *loop) {
    pthread_mutex_lock(&loop->lock);
    
    TimerObject *t = loop->timer_queue;
    while (t) {
        TimerObject *next = t->next;
        free(t);
        t = next;
    }
    loop->timer_queue = NULL;
    
    pthread_mutex_unlock(&loop->lock);
}
```

---

## Phase 13: Exception Propagation

### 13.1 Complete Exception Handling

```c
// Store exception in fiber for later retrieval
typedef struct {
    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_traceback;
} StoredException;

// Capture current exception
static StoredException* capture_exception(void) {
    if (!PyErr_Occurred()) return NULL;
    
    StoredException *stored = malloc(sizeof(StoredException));
    PyErr_Fetch(&stored->exc_type, &stored->exc_value, &stored->exc_traceback);
    PyErr_NormalizeException(&stored->exc_type, &stored->exc_value, 
                             &stored->exc_traceback);
    return stored;
}

// Restore exception in fiber context
static void restore_exception(StoredException *stored) {
    if (!stored) return;
    PyErr_Restore(stored->exc_type, stored->exc_value, stored->exc_traceback);
    free(stored);
}

// Free without restoring
static void free_exception(StoredException *stored) {
    if (!stored) return;
    Py_XDECREF(stored->exc_type);
    Py_XDECREF(stored->exc_value);
    Py_XDECREF(stored->exc_traceback);
    free(stored);
}

// Updated fiber_resume with proper exception handling
static void fiber_resume(FiberObject *f) {
    if (!f || f->state == FIBER_STATE_DONE) return;
    
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    PyObject *result = NULL;
    
    // Restore any stored exception before resuming
    if (f->exception) {
        restore_exception(f->exception);
        f->exception = NULL;
    }
    
    if (f->type == FIBER_ASYNC) {
        // === ASYNC: Resume coroutine ===
        if (f->awaited == NULL) {
            result = PyObject_Send(f->coro, Py_None);
        } else {
            result = PyObject_Send(f->coro, f->awaited);
            Py_DECREF(f->awaited);
            f->awaited = NULL;
        }
        
        if (result == NULL) {
            // Exception occurred in coroutine
            if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
                PyErr_Clear();
                f->state = FIBER_STATE_DONE;
                Py_DECREF(f->coro);
                f->coro = NULL;
            } else if (PyErr_ExceptionMatches(PyExc_CancelledError)) {
                PyErr_Clear();
                f->state = FIBER_STATE_CANCELLED;
                Py_DECREF(f->coro);
                f->coro = NULL;
            } else {
                // Store exception for propagation
                f->exception = capture_exception();
                f->state = FIBER_STATE_DONE;
                Py_DECREF(f->coro);
                f->coro = NULL;
            }
            PyGILState_Release(gstate);
            return;
        }
        
        if (PyObject_TypeCheck(result, &FutureType)) {
            FutureObject *fut = (FutureObject*)result;
            f->awaited = (PyObject*)fut;
            f->state = FIBER_STATE_SUSPENDED;
            fut->fiber = f;
            loop_add_pending(_current_loop, f);
        } else if (result == Py_None) {
            Py_DECREF(result);
            loop_add_ready(_current_loop, f);
        } else {
            // Unknown yield - store and continue
            f->awaited = result;
            loop_add_pending(_current_loop, f);
        }
        
    } else {
        // === SYNC: Call function ===
        if (f->func != NULL) {
            f->result = PyObject_CallObject((PyObject*)f->func, f->args);
            if (f->result == NULL) {
                f->exception = capture_exception();
            }
            f->state = FIBER_STATE_DONE;
            Py_DECREF(f->args);
            f->args = NULL;
        }
    }
    
    PyGILState_Release(gstate);
}
```

---

## Phase 14: Missing Python Helpers (C Implementation)

### 14.1 Atomic Operations for Python

```c
// Global atomic counters
static atomic_int _task_count = 0;
static atomic_int _pending_count = 0;

// Increment task count
static PyObject* atomic_task_inc(PyObject *self) {
    int new_val = atomic_fetch_add(&_task_count, 1) + 1;
    return PyLong_FromLong(new_val);
}

// Decrement task count
static PyObject* atomic_task_dec(PyObject *self) {
    int new_val = atomic_fetch_sub(&_task_count, 1) - 1;
    return PyLong_FromLong(new_val);
}

// Get task count
static PyObject* atomic_task_count(PyObject *self) {
    return PyLong_FromLong(atomic_load(&_task_count));
}

// Check if all tasks complete
static PyObject* atomic_all_tasks_complete(PyObject *self) {
    int count = atomic_load(&_task_count);
    return PyBool_FromLong(count == 0);
}

// Yield execution to scheduler
static PyObject* yield_execution_py(PyObject *self) {
    FiberObject *current = get_current_fiber();
    if (current && current->state == FIBER_STATE_RUNNING) {
        // Move to end of ready queue
        pthread_mutex_lock(&_current_loop->lock);
        current->state = FIBER_STATE_SUSPENDED;
        loop_add_pending(_current_loop, current);
        pthread_mutex_unlock(&_current_loop->lock);
        
        // Wait to be resumed
        pthread_mutex_lock(&_current_loop->lock);
        while (current->state == FIBER_STATE_SUSPENDED) {
            pthread_cond_wait(&_current_loop->cond, &_current_loop->lock);
        }
        pthread_mutex_unlock(&_current_loop->lock);
    }
    Py_RETURN_NONE;
}

// Thread pool initialization
static ThreadPoolObject *_global_pool = NULL;

static PyObject* init_scheduler_py(PyObject *self, PyObject *args) {
    int num_workers = 0;  // 0 = auto (CPU count)
    
    if (!PyArg_ParseTuple(args, "|i", &num_workers)) {
        return NULL;
    }
    
    if (num_workers <= 0) {
        num_workers = sysconf(_SC_NPROCESSORS_ONLN);
    }
    
    if (_global_pool) {
        Py_RETURN_NONE;  // Already initialized
    }
    
    _global_pool = pool_create(num_workers);
    if (!_global_pool) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create thread pool");
        return NULL;
    }
    
    return PyLong_FromLong(num_workers);
}

static PyObject* shutdown_scheduler_py(PyObject *self, PyObject *args) {
    int wait = 1;
    
    if (!PyArg_ParseTuple(args, "|p", &wait)) {
        return NULL;
    }
    
    if (_global_pool) {
        pool_shutdown(_global_pool, wait);
        _global_pool = NULL;
    }
    
    Py_RETURN_NONE;
}

static PyObject* num_workers_py(PyObject *self) {
    if (!_global_pool) {
        return PyLong_FromLong(0);
    }
    return PyLong_FromLong(_global_pool->num_workers);
}
```

---

## Notes

1. **No asyncio dependency** - All scheduling done in C
2. **Thread-local event loop** - Each thread has its own loop
3. **Work-stealing** - Workers can steal fibers from other threads
4. **Lock-free queues** - Use C11 atomics where possible
5. **GIL management** - Acquire/release GIL appropriately in C code
6. **Unified scheduler** - Both sync and async share same fiber pool
7. **Backward compatible** - Existing Task/Sync code continues to work
8. **Memory safety** - All allocated resources freed in cleanup paths
9. **Cancellation** - Cooperative cancellation via tokens
10. **Graceful shutdown** - Three-phase shutdown protocol
11. **Exception propagation** - Exceptions stored and re-raised in caller context
12. **I/O integration** - epoll/kqueue for async I/O operations
13. **Sync primitives** - Lock, Event, Semaphore, Condition for coordination
14. **Fiber introspection** - Debug info for all fibers in system
15. **Atomic operations** - Lock-free counters for task tracking
