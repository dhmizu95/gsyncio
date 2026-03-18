# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False

"""
_gsyncio_core.pyx - Cython wrapper for gsyncio C core

This module provides Python bindings to the high-performance C core
of gsyncio, including fibers, scheduler, futures, channels, waitgroups,
and select operations.
"""

from libc.stdlib cimport malloc, free
from libc.stdint cimport uint64_t, int64_t
from libc.string cimport memset
from cpython.ref cimport PyObject, Py_INCREF, Py_DECREF
from cpython.object cimport PyObject

# Use size_t from libc.stddef
cdef extern from "stddef.h":
    ctypedef unsigned long size_t

# pthread types
cdef extern from "pthread.h":
    ctypedef long pthread_mutex_t
    ctypedef long pthread_cond_t
    ctypedef unsigned long pthread_t

# sigjmp_buf from setjmp
cdef extern from "setjmp.h":
    ctypedef struct sigjmp_buf:
        pass

# Forward declarations for C types
cdef extern from "fiber.h":
    ctypedef enum fiber_state_t:
        FIBER_NEW
        FIBER_READY
        FIBER_RUNNING
        FIBER_WAITING
        FIBER_COMPLETED
        FIBER_CANCELLED
    
    ctypedef struct fiber_t:
        uint64_t id
        fiber_state_t state
        void* stack_base
        void* stack_ptr
        size_t stack_size
        size_t stack_capacity
        void (*func)(void*)
        void* arg
        void* result
        fiber_t* parent
        fiber_t* next_ready
        fiber_t* prev_ready
        int affinity
        void* pool
        const char* name
        sigjmp_buf context
        sigjmp_buf* sched_jump
        void* waiting_on
    
    int fiber_init()
    void fiber_cleanup()
    fiber_t* fiber_create(void (*func)(void*), void* arg, size_t stack_size)
    void fiber_free(fiber_t* fiber)
    fiber_t* fiber_current()
    void fiber_yield()
    void fiber_resume(fiber_t* fiber)
    void fiber_park()
    void fiber_unpark(fiber_t* fiber)
    uint64_t fiber_id(fiber_t* fiber)
    fiber_state_t fiber_state(fiber_t* fiber)
    int fiber_is_parked(fiber_t* fiber)

cdef extern from "scheduler.h":
    ctypedef struct scheduler_config_t:
        size_t num_workers
        size_t max_fibers
        size_t stack_size
        int work_stealing
    
    ctypedef struct scheduler_stats_t:
        uint64_t total_fibers_created
        uint64_t total_fibers_completed
        uint64_t total_context_switches
        uint64_t total_work_steals
        uint64_t current_active_fibers
        uint64_t current_ready_fibers
    
    ctypedef struct scheduler_t:
        pass
    
    scheduler_t* g_scheduler
    
    int scheduler_init(scheduler_config_t* config)
    void scheduler_shutdown(int wait_for_completion)
    scheduler_t* scheduler_get()
    uint64_t scheduler_spawn(void (*entry)(void*), void* user_data)
    void scheduler_schedule(fiber_t* f, int worker_id)
    void scheduler_block(void* reason)
    void scheduler_unblock(fiber_t* f)
    void scheduler_yield()
    void scheduler_wait(fiber_t* f)
    void scheduler_wait_all()
    void scheduler_get_stats(scheduler_stats_t* stats)
    int scheduler_current_worker()
    size_t scheduler_num_workers()
    void scheduler_run()
    void scheduler_stop()

cdef extern from "future.h":
    ctypedef enum future_state:
        FUTURE_PENDING
        FUTURE_READY
        FUTURE_EXCEPTION
    
    ctypedef struct future_t:
        future_state state
        void* result
        void* exception
        int refcount
    
    future_t* future_create()
    void future_destroy(future_t* f)
    void future_incref(future_t* f)
    void future_decref(future_t* f)
    int future_is_done(future_t* f)
    int future_has_exception(future_t* f)
    int future_set_result(future_t* f, void* result)
    void* future_result(future_t* f)
    int future_set_exception(future_t* f, void* exc)
    void* future_exception(future_t* f)
    void future_wait(future_t* f)
    void future_await(future_t* f)

cdef extern from "channel.h":
    ctypedef struct channel_t:
        uint64_t id
        size_t capacity
        size_t size
        int closed
        int refcount
    
    channel_t* channel_create(size_t capacity)
    void channel_destroy(channel_t* ch)
    void channel_incref(channel_t* ch)
    void channel_decref(channel_t* ch)
    int channel_send(channel_t* ch, void* value)
    void* channel_recv(channel_t* ch)
    int channel_try_send(channel_t* ch, void* value)
    int channel_try_recv(channel_t* ch, void** out_value)
    int channel_close(channel_t* ch)
    int channel_is_closed(channel_t* ch)
    size_t channel_size(channel_t* ch)
    int channel_is_empty(channel_t* ch)
    int channel_is_full(channel_t* ch)
    uint64_t channel_id(channel_t* ch)

cdef extern from "waitgroup.h":
    ctypedef struct waitgroup_t:
        int64_t counter
        int refcount
    
    waitgroup_t* waitgroup_create()
    void waitgroup_destroy(waitgroup_t* wg)
    void waitgroup_incref(waitgroup_t* wg)
    void waitgroup_decref(waitgroup_t* wg)
    int waitgroup_add(waitgroup_t* wg, int64_t delta)
    void waitgroup_done(waitgroup_t* wg)
    void waitgroup_wait(waitgroup_t* wg)
    int64_t waitgroup_counter(waitgroup_t* wg)

cdef extern from "fiber_pool.h":
    ctypedef struct fiber_pool_t:
        size_t capacity
        size_t available
        size_t allocated
    
    fiber_pool_t* fiber_pool_create(size_t initial_size)
    void fiber_pool_destroy(fiber_pool_t* pool)
    fiber_t* fiber_pool_alloc(fiber_pool_t* pool)
    void fiber_pool_free(fiber_pool_t* pool, fiber_t* fiber)
    size_t fiber_pool_available(fiber_pool_t* pool)
    size_t fiber_pool_allocated(fiber_pool_t* pool)
    size_t fiber_pool_capacity(fiber_pool_t* pool)

# ============================================
# Python-visible classes and functions
# ============================================

cdef class Future:
    """Python wrapper for future_t"""
    cdef future_t* _future
    
    def __cinit__(self):
        self._future = future_create()
        if not self._future:
            raise MemoryError("Failed to create future")
    
    def __dealloc__(self):
        if self._future:
            future_decref(self._future)
    
    @property
    def done(self):
        """Check if future is done"""
        return future_is_done(self._future) != 0
    
    @property
    def cancelled(self):
        """Check if future is cancelled (not implemented)"""
        return False
    
    def result(self, timeout=None):
        """Get the result of the future"""
        if not future_is_done(self._future):
            future_wait(self._future)
        
        if future_has_exception(self._future):
            exc = <object>future_exception(self._future)
            if exc is not None:
                raise exc
            raise RuntimeError("Future completed with exception")
        
        return <object>future_result(self._future)
    
    def exception(self, timeout=None):
        """Get the exception of the future"""
        if not future_is_done(self._future):
            future_wait(self._future)
        
        return <object>future_exception(self._future)
    
    def set_result(self, result):
        """Set the result of the future"""
        Py_INCREF(result)
        if future_set_result(self._future, <void*>result) != 0:
            Py_DECREF(result)
            raise RuntimeError("Future already completed")
    
    def set_exception(self, exc):
        """Set the exception of the future"""
        Py_INCREF(exc)
        if future_set_exception(self._future, <void*>exc) != 0:
            Py_DECREF(exc)
            raise RuntimeError("Future already completed")
    
    def __await__(self):
        """Make future awaitable"""
        if not future_is_done(self._future):
            future_await(self._future)
        return self.result()


cdef class Channel:
    """Python wrapper for channel_t"""
    cdef channel_t* _channel
    
    def __cinit__(self, size_t capacity=0):
        self._channel = channel_create(capacity)
        if not self._channel:
            raise MemoryError("Failed to create channel")
    
    def __dealloc__(self):
        if self._channel:
            channel_decref(self._channel)
    
    @property
    def capacity(self):
        """Channel buffer capacity"""
        return self._channel.capacity if self._channel else 0
    
    @property
    def size(self):
        """Current number of items in buffer"""
        return channel_size(self._channel) if self._channel else 0
    
    @property
    def closed(self):
        """Check if channel is closed"""
        return channel_is_closed(self._channel) != 0 if self._channel else True
    
    async def send(self, value):
        """Send value to channel (async)"""
        Py_INCREF(value)
        if channel_send(self._channel, <void*>value) != 0:
            Py_DECREF(value)
            raise RuntimeError("Channel closed")
    
    async def recv(self):
        """Receive value from channel (async)"""
        cdef void* value = channel_recv(self._channel)
        if value == NULL:
            if channel_is_closed(self._channel):
                raise StopAsyncIteration("Channel closed")
            return None
        return <object>value
    
    def send_nowait(self, value):
        """Send value without blocking"""
        Py_INCREF(value)
        if channel_try_send(self._channel, <void*>value) != 0:
            Py_DECREF(value)
            return False
        return True
    
    def recv_nowait(self):
        """Receive value without blocking"""
        cdef void* value
        if channel_try_recv(self._channel, &value) != 0:
            return None
        return <object>value
    
    def close(self):
        """Close the channel"""
        channel_close(self._channel)
    
    def __len__(self):
        return self.size
    
    def __bool__(self):
        return not self.closed


cdef class WaitGroup:
    """Python wrapper for waitgroup_t"""
    cdef waitgroup_t* _wg
    
    def __cinit__(self):
        self._wg = waitgroup_create()
        if not self._wg:
            raise MemoryError("Failed to create waitgroup")
    
    def __dealloc__(self):
        if self._wg:
            waitgroup_decref(self._wg)
    
    def add(self, int64_t delta=1):
        """Add delta to counter"""
        if waitgroup_add(self._wg, delta) != 0:
            raise RuntimeError("WaitGroup counter would be negative")
    
    def done(self):
        """Decrement counter by 1"""
        waitgroup_done(self._wg)
    
    async def wait(self):
        """Wait for counter to reach zero"""
        waitgroup_wait(self._wg)
    
    @property
    def counter(self):
        """Current counter value"""
        return waitgroup_counter(self._wg)


# ============================================
# Module-level functions
# ============================================

def init_scheduler(size_t num_workers=0, size_t max_fibers=1000000, int work_stealing=1):
    """Initialize the gsyncio scheduler"""
    cdef scheduler_config_t config
    config.num_workers = num_workers if num_workers > 0 else 0  # 0 = auto-detect
    config.max_fibers = max_fibers
    config.stack_size = 8192  # 8KB default
    config.work_stealing = work_stealing
    
    if scheduler_init(&config) != 0:
        raise RuntimeError("Failed to initialize scheduler")
    
    fiber_init()

def shutdown_scheduler(int wait=1):
    """Shutdown the gsyncio scheduler"""
    scheduler_shutdown(wait)
    fiber_cleanup()

def get_scheduler_stats():
    """Get scheduler statistics"""
    cdef scheduler_stats_t stats
    scheduler_get_stats(&stats)
    return {
        'total_fibers_created': stats.total_fibers_created,
        'total_fibers_completed': stats.total_fibers_completed,
        'total_context_switches': stats.total_context_switches,
        'total_work_steals': stats.total_work_steals,
        'current_active_fibers': stats.current_active_fibers,
        'current_ready_fibers': stats.current_ready_fibers,
    }

cdef object _fiber_wrapper_func
cdef object _fiber_wrapper_args

cdef void _c_fiber_entry(void* arg) noexcept:
    """C callback for fiber execution"""
    global _fiber_wrapper_func, _fiber_wrapper_args
    try:
        _fiber_wrapper_func(*_fiber_wrapper_args)
    except Exception as e:
        import sys
        print(f"Fiber exception: {e}", file=sys.stderr)

def spawn(func, *args):
    """Spawn a new fiber/task using fiber pool when available"""
    global _fiber_wrapper_func, _fiber_wrapper_args
    
    # Check if scheduler is initialized and has workers
    if scheduler_num_workers() > 0:
        _fiber_wrapper_func = func
        _fiber_wrapper_args = args
        fid = scheduler_spawn(_c_fiber_entry, NULL)
        if fid > 0:
            return fid
    
    # Fallback to lightweight thread
    import threading
    t = threading.Thread(target=lambda: func(*args), daemon=True)
    t.start()
    return t

def sleep_ms(int ms):
    """Sleep for milliseconds - uses asyncio for non-blocking sleep"""
    # Delegate to asyncio for proper async sleep
    # This is called from async context, so we return a coroutine
    import asyncio
    async def _sleep():
        await asyncio.sleep(ms / 1000.0)
    return _sleep()

def current_fiber_id():
    """Get current fiber ID"""
    cdef fiber_t* f = fiber_current()
    return fiber_id(f) if f else 0

def yield_execution():
    """Yield execution to scheduler"""
    fiber_yield()

def num_workers():
    """Get number of worker threads"""
    return scheduler_num_workers()

# Native networking initialized in scheduler
