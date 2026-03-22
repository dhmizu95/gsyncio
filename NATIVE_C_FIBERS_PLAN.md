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
├── _gsyncio_core.c          # Native C fiber scheduler
├── _gsyncio_core.h          # C header file
├── task.py                  # run(), task(), sync()
├── channel.py               # Chan (sync + async API)
├── async_.py                # sleep(), gather(), wait_for()
└── setup.py                 # Build C extension
```

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

- [ ] **Phase 1: C Extension**
  - [ ] Define Fiber, Future, EventLoop structs
  - [ ] Implement fiber_create, fiber_resume, fiber_park
  - [ ] Implement loop_create, loop_run, loop_add_ready
  - [ ] Implement future_create, future_set_result
  - [ ] Implement timer_schedule, timer_check
  - [ ] Create Python module definition

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

---

## Performance Goals

| Operation | Target | Notes |
|-----------|--------|-------|
| Fiber create | < 1 µs | Pure C allocation |
| Fiber park/unpark | < 500 ns | pthread_cond_wait |
| Channel send/recv | < 2 µs | Lock-free where possible |
| sleep(0) yield | < 500 ns | Direct scheduler call |
| Task spawn | < 1 µs | C atomic increment |

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

## Notes

1. **No asyncio dependency** - All scheduling done in C
2. **Thread-local event loop** - Each thread has its own loop
3. **Work-stealing** - Workers can steal fibers from other threads
4. **Lock-free queues** - Use C11 atomics where possible
5. **GIL management** - Acquire/release GIL appropriately in C code
6. **Unified scheduler** - Both sync and async share same fiber pool
7. **Backward compatible** - Existing Task/Sync code continues to work
