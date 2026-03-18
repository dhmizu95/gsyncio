# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False

"""
_gsyncio_core.pyx - Cython wrapper for gsyncio C core

This module provides Python bindings to the high-performance C core
of gsyncio, including fibers, scheduler, futures, channels, waitgroups,
select operations, and task/sync model.
"""

from libc.stdlib cimport malloc, free
from libc.stdint cimport uint64_t, int64_t
from libc.string cimport memset, memcpy
from cpython.ref cimport PyObject, Py_INCREF, Py_DECREF, Py_XINCREF, Py_XDECREF
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

# ============================================
# Fiber declarations
# ============================================
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

# ============================================
# Scheduler declarations
# ============================================
cdef extern from "scheduler.h":
    ctypedef struct scheduler_config_t:
        size_t num_workers
        size_t max_fibers
        size_t stack_size
        int work_stealing
        int backend
        size_t io_uring_entries

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
    void scheduler_sleep_ns(uint64_t ns)
    
    # Worker management
    void scheduler_check_worker_scaling()
    void scheduler_set_auto_scaling(int enabled)
    void scheduler_set_energy_efficient_mode(int enabled)
    double scheduler_get_worker_utilization()
    size_t scheduler_get_recommended_workers()

# ============================================
# Future declarations
# ============================================
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

# ============================================
# Channel declarations
# ============================================
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

# ============================================
# WaitGroup declarations
# ============================================
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

# ============================================
# Select declarations
# ============================================
cdef extern from "select.h":
    ctypedef enum select_case_type:
        SELECT_CASE_RECV
        SELECT_CASE_SEND
        SELECT_CASE_DEFAULT

    ctypedef struct select_case_t:
        select_case_type type
        channel_t* channel
        void* value
        void* recv_value
        int executed

    ctypedef struct select_result_t:
        int case_index
        select_case_t* case_info
        void* value
        int success

    ctypedef struct select_state_t:
        select_case_t* cases
        size_t case_count
        select_result_t result
        void* waiting_fiber
        int refcount

    select_state_t* select_create(size_t case_count)
    void select_destroy(select_state_t* sel)
    void select_set_recv(select_state_t* sel, size_t index, channel_t* ch)
    void select_set_send(select_state_t* sel, size_t index, channel_t* ch, void* value)
    void select_set_default(select_state_t* sel, size_t index)
    select_result_t select_try(select_state_t* sel)
    select_result_t select_execute(select_state_t* sel)
    select_result_t select_result(select_state_t* sel)

# ============================================
# Task declarations
# ============================================
cdef extern from "task.h":
    ctypedef enum task_state:
        TASK_STATE_RUNNING
        TASK_STATE_COMPLETED
        TASK_STATE_CANCELLED

    ctypedef struct task_handle_t:
        uint64_t fiber_id
        uint64_t task_id
        task_state state
        void* result
        void* exception

    ctypedef struct task_registry_t:
        size_t active_count
        size_t task_count
        size_t completion_count

    ctypedef struct task_batch_t:
        size_t count
        size_t capacity

    task_registry_t* task_registry_create()
    void task_registry_destroy(task_registry_t* reg)
    task_handle_t* task_spawn(task_registry_t* reg, void (*func)(void*), void* arg)
    void task_sync(task_registry_t* reg)
    int task_sync_timeout(task_registry_t* reg, uint64_t timeout_ns)
    size_t task_count(task_registry_t* reg)
    size_t task_completed_count(task_registry_t* reg)
    task_registry_t* task_get_registry()
    void task_set_registry(task_registry_t* reg)
    void task_reset_registry(task_registry_t* reg)
    
    # Batch operations
    task_batch_t* task_batch_create(size_t capacity)
    void task_batch_destroy(task_batch_t* batch)
    int task_batch_add(task_batch_t* batch, void (*func)(void*), void* arg)
    int task_batch_spawn(task_batch_t* batch, task_registry_t* reg)

# ============================================
# Python-visible classes
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


cdef class SelectState:
    """Python wrapper for select_state_t"""
    cdef select_state_t* _sel

    def __cinit__(self, size_t case_count):
        if case_count == 0:
            raise ValueError("Case count must be > 0")
        self._sel = select_create(case_count)
        if not self._sel:
            raise MemoryError("Failed to create select state")

    def __dealloc__(self):
        if self._sel:
            select_destroy(self._sel)

    def set_recv(self, size_t index, Channel ch):
        """Set up a receive case"""
        if self._sel and ch._channel:
            select_set_recv(self._sel, index, ch._channel)

    def set_send(self, size_t index, Channel ch, value):
        """Set up a send case"""
        if self._sel and ch._channel:
            Py_INCREF(value)
            select_set_send(self._sel, index, ch._channel, <void*>value)

    def set_default(self, size_t index):
        """Set up a default case"""
        if self._sel:
            select_set_default(self._sel, index)

    def try_select(self):
        """Try select without blocking"""
        if not self._sel:
            return None
        cdef select_result_t result = select_try(self._sel)
        if result.success:
            return {
                'case_index': result.case_index,
                'value': <object>result.value if result.value else None,
                'success': True
            }
        return None

    def execute(self):
        """Execute select (blocks until a case is ready)"""
        if not self._sel:
            return None
        cdef select_result_t result = select_execute(self._sel)
        if result.success:
            return {
                'case_index': result.case_index,
                'value': <object>result.value if result.value else None,
                'success': True
            }
        return None


cdef class TaskRegistry:
    """Python wrapper for task_registry_t"""
    cdef task_registry_t* _reg

    def __cinit__(self):
        self._reg = task_registry_create()
        if not self._reg:
            raise MemoryError("Failed to create task registry")

    def __dealloc__(self):
        if self._reg:
            task_registry_destroy(self._reg)

    def spawn(self, func, *args):
        """Spawn a new task"""
        cdef object payload = (func, args)
        Py_INCREF(payload)
        cdef task_handle_t* handle = task_spawn(self._reg, _c_task_entry, <void*>payload)
        if not handle:
            Py_DECREF(payload)
            raise RuntimeError("Failed to spawn task")
        cdef uint64_t fid = handle.fiber_id
        return fid

    def spawn_batch(self, tasks):
        """Spawn multiple tasks in a batch (optimized)"""
        cdef task_batch_t* batch_ptr = task_batch_create(len(tasks))
        if not batch_ptr:
            raise MemoryError("Failed to create task batch")
         
        cdef object payload
        cdef void* arg_ptr
        
        try:
            # Add all tasks to batch
            for func, args in tasks:
                payload = (func, args)
                Py_INCREF(payload)
                arg_ptr = <void*>payload
                if task_batch_add(batch_ptr, _c_task_entry, arg_ptr) != 0:
                    Py_DECREF(payload)
                    raise RuntimeError("Failed to add task to batch")

            # Spawn all at once
            if task_batch_spawn(batch_ptr, self._reg) != 0:
                raise RuntimeError("Failed to spawn batch")

            # Return count of spawned tasks
            return batch_ptr.count
        finally:
            task_batch_destroy(batch_ptr)

    def sync(self):
        """Wait for all tasks to complete"""
        task_sync(self._reg)

    def sync_timeout(self, float timeout_s):
        """Wait for all tasks with timeout"""
        cdef uint64_t timeout_ns = <uint64_t>(timeout_s * 1000000000)
        return task_sync_timeout(self._reg, timeout_ns) == 0

    def reset(self):
        """Reset registry state for reuse"""
        task_reset_registry(self._reg)

    @property
    def active_count(self):
        """Number of active tasks"""
        return self._reg.active_count if self._reg else 0

    @property
    def task_count(self):
        """Total tasks spawned"""
        return self._reg.task_count if self._reg else 0

    @property
    def completed_count(self):
        """Number of completed tasks"""
        return self._reg.completion_count if self._reg else 0


cdef class TaskBatch:
    """Python wrapper for task_batch_t - batch task spawning"""
    cdef task_batch_t* _batch
    cdef TaskRegistry _registry
    cdef list _payloads

    def __cinit__(self, TaskRegistry registry, size_t capacity=64):
        self._batch = task_batch_create(capacity)
        if not self._batch:
            raise MemoryError("Failed to create task batch")
        self._registry = registry
        self._payloads = []

    def __dealloc__(self):
        if self._batch:
            task_batch_destroy(self._batch)

    def add(self, func, *args):
        """Add a task to the batch"""
        cdef object payload
        cdef void* arg_ptr
         
        payload = (func, args)
        self._payloads.append(payload)
        Py_INCREF(payload)
        arg_ptr = <void*>payload
        if task_batch_add(self._batch, _c_task_entry, arg_ptr) != 0:
            raise RuntimeError("Failed to add task to batch")
        return len(self._payloads) - 1

    def spawn(self):
        """Spawn all tasks in the batch"""
        if task_batch_spawn(self._batch, self._registry._reg) != 0:
            raise RuntimeError("Failed to spawn batch")
        
        # Return count of spawned tasks
        return self._batch.count

    def __len__(self):
        return self._batch.count if self._batch else 0


cdef void _c_task_entry(void* arg) noexcept nogil:
    """C callback for task entry - arg is a Python tuple (func, args)
    
    Optimized for performance - minimal overhead in hot path.
    Exception handling is done inline without try/except overhead.
    """
    if arg != NULL:
        with gil:
            payload = <object>arg
            # Fast path: direct call without try/except overhead
            func = payload[0]
            args = payload[1]
            try:
                func(*args)
            except:
                # Only import sys on exception (rare case)
                import sys
                print(f"Task exception: {sys.exc_info()[1]}", file=sys.stderr)
            finally:
                # Clear payload reference
                del payload


cdef void _c_fiber_entry(void* arg) noexcept nogil:
    """C callback for fiber entry - arg is a Python tuple (func, args)
    
    Optimized for performance - minimal overhead in hot path.
    """
    if arg != NULL:
        with gil:
            payload = <object>arg
            func = payload[0]
            args = payload[1]
            try:
                func(*args)
            except:
                import sys
                print(f"Fiber exception: {sys.exc_info()[1]}", file=sys.stderr)
            finally:
                del payload


# ============================================
# Module-level functions
# ============================================

# Global task registry
_task_registry = None

# Object pool for task payloads (reduces allocation overhead)
cdef object _payload_pool = []
cdef size_t _payload_pool_size = 0
cdef size_t _PAYLOAD_POOL_MAX_SIZE = 1024
import threading
cdef object _payload_pool_lock = threading.Lock()  # Lock for protecting payload pool access

def init_scheduler(size_t num_workers=0, size_t max_fibers=1000000, int work_stealing=1):
    """Initialize the gsyncio scheduler"""
    global _task_registry
    cdef scheduler_config_t config
    config.num_workers = num_workers
    config.max_fibers = max_fibers
    config.stack_size = 2048
    config.work_stealing = work_stealing
    config.backend = 0
    config.io_uring_entries = 256

    if scheduler_init(&config) != 0:
        raise RuntimeError("Failed to initialize scheduler")

    fiber_init()

    # Create global task registry
    _task_registry = TaskRegistry()


def shutdown_scheduler(int wait=1):
    """Shutdown the gsyncio scheduler"""
    global _task_registry
    if wait and _task_registry:
        _task_registry.sync()
    scheduler_shutdown(wait)
    fiber_cleanup()
    # Reset registry for next use
    if _task_registry:
        _task_registry.reset()


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


# Object pool for task payloads (reduces allocation overhead)
cdef object _get_payload(func, args):
    """Get a payload from pool or create new one"""
    global _payload_pool, _payload_pool_size, _payload_pool_lock
    
    with _payload_pool_lock:
        # Reuse from pool if available
        if _payload_pool_size > 0:
            payload = _payload_pool.pop()
            _payload_pool_size -= 1
            payload[0] = func
            payload[1] = args
            return payload
        
        # Create new payload
        return [func, args]


cdef void _return_payload(payload) noexcept:
    """Return payload to pool for reuse"""
    global _payload_pool, _payload_pool_size, _payload_pool_lock
    
    with _payload_pool_lock:
        # Clear references
        payload[0] = None
        payload[1] = None
        
        # Return to pool if not full
        if _payload_pool_size < _PAYLOAD_POOL_MAX_SIZE:
            _payload_pool.append(payload)
            _payload_pool_size += 1


def spawn(func, *args):
    """Spawn a new fiber/task - optimized with object pooling"""
    global _task_registry, _payload_pool, _payload_pool_size

    # Initialize scheduler if not already initialized
    if g_scheduler == NULL:
        init_scheduler(num_workers=4)  # Default to 4 workers

    # Use pooled payload object (reduces allocation overhead)
    cdef object payload = _get_payload(func, args)
    Py_INCREF(payload)
    cdef uint64_t fid = scheduler_spawn(_c_fiber_entry, <void*>payload)
    if fid == 0:
        Py_DECREF(payload)
        _return_payload(payload)
        raise RuntimeError("Failed to spawn fiber")
    return fid


def spawn_batch(funcs_and_args):
    """Spawn multiple tasks in a batch - optimized for bulk operations

    Args:
        funcs_and_args: List of (func, args) tuples

    Returns:
        List of fiber IDs

    Example:
        >>> spawn_batch([(func1, (arg1,)), (func2, (arg2,))])
        [1, 2]
    """
    global _payload_pool, _payload_pool_size

    if g_scheduler == NULL:
        init_scheduler(num_workers=4)

    cdef size_t count = len(funcs_and_args)
    cdef list results = []
    cdef object payload
    cdef uint64_t fid

    # Pre-allocate results list
    results = [None] * count

    # Spawn all tasks using pooled payloads
    for i, (func, args) in enumerate(funcs_and_args):
        # Use pooled payload (reduces allocation overhead)
        payload = _get_payload(func, args)
        Py_INCREF(payload)
        fid = scheduler_spawn(_c_fiber_entry, <void*>payload)
        if fid == 0:
            Py_DECREF(payload)
            _return_payload(payload)
            raise RuntimeError(f"Failed to spawn fiber {i}")
        results[i] = fid

    return results


def spawn_batch_fast(funcs_and_args):
    """Ultra-fast batch spawn - minimal error checking

    For maximum performance when you know all spawns will succeed.
    Does not return fiber IDs (saves allocation overhead).

    Args:
        funcs_and_args: List of (func, args) tuples
    """
    if g_scheduler == NULL:
        init_scheduler(num_workers=4)

    cdef object payload

    # Spawn all tasks without error checking or return values
    for func, args in funcs_and_args:
        payload = _get_payload(func, args)
        Py_INCREF(payload)
        scheduler_spawn(_c_fiber_entry, <void*>payload)


def sleep_ns(uint64_t ns):
    """Sleep for nanoseconds using native timer (fast path)"""
    scheduler_sleep_ns(ns)


def sleep_ms(int ms):
    """Sleep for milliseconds using native timer"""
    scheduler_sleep_ns(ms * 1000000)


def sleep_us(int us):
    """Sleep for microseconds using native timer"""
    scheduler_sleep_ns(us * 1000)


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


def check_worker_scaling():
    """Check if worker scaling is needed"""
    scheduler_check_worker_scaling()


def set_auto_scaling(enabled):
    """Enable or disable automatic worker scaling"""
    scheduler_set_auto_scaling(1 if enabled else 0)


def set_energy_efficient_mode(enabled):
    """Enable energy-efficient mode (fewer workers when idle)"""
    scheduler_set_energy_efficient_mode(1 if enabled else 0)


def get_worker_utilization():
    """Get worker utilization percentage (0-100)"""
    return scheduler_get_worker_utilization()


def get_recommended_workers():
    """Get recommended number of workers based on current workload"""
    return scheduler_get_recommended_workers()


def task(func, *args):
    """Spawn a task using global task registry"""
    global _task_registry
    if not _task_registry:
        init_scheduler()
    return _task_registry.spawn(func, *args)


def task_batch():
    """Create a task batch for efficient bulk spawning"""
    global _task_registry
    if not _task_registry:
        init_scheduler()
    return TaskBatch(_task_registry)


def sync():
    """Wait for all tasks to complete"""
    global _task_registry
    if _task_registry:
        _task_registry.sync()


def sync_timeout(float timeout_s):
    """Wait for all tasks with timeout"""
    global _task_registry
    if not _task_registry:
        return True
    return _task_registry.sync_timeout(timeout_s)


def task_count():
    """Get number of active tasks"""
    global _task_registry
    if not _task_registry:
        return 0
    return _task_registry.active_count


def task_completed_count():
    """Get number of completed tasks"""
    global _task_registry
    if not _task_registry:
        return 0
    return _task_registry.completed_count


def run(func, *args):
    """Run a function in the gsyncio runtime"""
    init_scheduler()
    try:
        func(*args)
        sync()
    finally:
        shutdown_scheduler(wait=True)


# Select helper functions
def select(*cases):
    """Execute a select on multiple channel operations"""
    if not cases:
        return None

    cdef size_t n = len(cases)
    cdef SelectState sel = SelectState(n)

    for i, case in enumerate(cases):
        if isinstance(case, dict) and 'channel' in case:
            ch = case['channel']
            if case.get('is_send', False):
                sel.set_send(i, ch, case.get('value'))
            else:
                sel.set_recv(i, ch)
        elif isinstance(case, str) and case == 'default':
            sel.set_default(i)

    result = sel.execute()
    return result