"""
gsyncio.core - Core interface for gsyncio

This module provides the core interface to gsyncio's fiber-based
concurrency primitives.
"""

import sys
import threading
from typing import Any, Callable, Coroutine, Optional, List
from contextlib import contextmanager

# Try to import the Cython extension, fall back to pure Python
try:
    from ._gsyncio_core import (
        Future,
        Channel,
        WaitGroup,
        TaskRegistry,
        TaskBatch,
        init_scheduler,
        shutdown_scheduler,
        get_scheduler_stats,
        spawn,
        spawn_direct,
        spawn_batch,
        spawn_batch_fast,
        spawn_batch_ultra_fast,
        sleep_ns,
        sleep_us,
        sleep_ms,
        current_fiber_id,
        yield_execution,
        num_workers,
        task,
        task_batch,
        sync,
        sync_timeout,
        task_count,
        task_completed_count,
        run,
        # Lock-free atomic operations
        atomic_task_count,
        atomic_inc_task_count,
        atomic_dec_task_count,
        atomic_all_tasks_complete,
        # Worker management
        check_worker_scaling,
        set_auto_scaling,
        set_energy_efficient_mode,
        get_worker_utilization,
        get_recommended_workers,
    )
    # Import _gsyncio_core module to access _task_registry dynamically
    from . import _gsyncio_core as __gsyncio_core
    _task_registry = property(lambda self: __gsyncio_core._task_registry)
    def _get_task_registry():
        return __gsyncio_core._task_registry
    _HAS_CYTHON = True
except ImportError:
    _HAS_CYTHON = False
    _task_registry = None
    def _get_task_registry():
        return None
    # Pure Python fallback implementations
    class Future:
        """Pure Python Future implementation"""
        def __init__(self):
            self._done = False
            self._result = None
            self._exception = None
            self._callbacks = []
            self._lock = threading.Lock()
        
        @property
        def done(self):
            return self._done
        
        @property
        def cancelled(self):
            return False
        
        def result(self, timeout=None):
            with self._lock:
                if not self._done:
                    # In pure Python, we can't really await
                    raise RuntimeError("Future not done")
                if self._exception:
                    raise self._exception
                return self._result
        
        def exception(self, timeout=None):
            with self._lock:
                if not self._done:
                    raise RuntimeError("Future not done")
                return self._exception
        
        def set_result(self, result):
            with self._lock:
                if self._done:
                    raise RuntimeError("Future already completed")
                self._result = result
                self._done = True
                for cb in self._callbacks:
                    cb(self)
        
        def set_exception(self, exc):
            with self._lock:
                if self._done:
                    raise RuntimeError("Future already completed")
                self._exception = exc
                self._done = True
                for cb in self._callbacks:
                    cb(self)
        
        def add_callback(self, cb):
            with self._lock:
                if self._done:
                    cb(self)
                else:
                    self._callbacks.append(cb)
        
        async def __await__(self):
            """Make future awaitable in asyncio context"""
            import asyncio
            if not self._done:
                # Create an asyncio event to wait for
                event = asyncio.Event()
                
                def on_done(f):
                    event.set()
                
                self.add_callback(on_done)
                await event.wait()
            return self.result()
    
    class Channel:
        """Pure Python Channel implementation - non-blocking"""
        def __init__(self, capacity=0):
            self._capacity = capacity
            self._queue = []
            self._closed = False
            self._waiters_send = []  # Fibers waiting to send
            self._waiters_recv = []  # Fibers waiting to receive
        
        @property
        def capacity(self):
            return self._capacity
        
        @property
        def size(self):
            return len(self._queue)
        
        @property
        def closed(self):
            return self._closed
        
        async def send(self, value):
            """Send value - non-blocking with async wait"""
            # Try non-blocking send first
            if self.send_nowait(value):
                return
            
            # Need to wait for space
            await self._wait_for_space()
            # Try again after waiting
            if not self.send_nowait(value):
                raise RuntimeError("Channel closed")
        
        async def recv(self):
            """Receive value - non-blocking with async wait"""
            # Try non-blocking receive first
            value = self.recv_nowait()
            if value is not None:
                return value
            
            # Need to wait for data
            await self._wait_for_data()
            # Try again after waiting
            value = self.recv_nowait()
            if value is None and self._closed:
                raise StopAsyncIteration("Channel closed")
            return value
        
        async def _wait_for_space(self):
            """Wait for space in channel (async)"""
            import asyncio
            event = asyncio.Event()
            self._waiters_send.append(event)
            try:
                await event.wait()
            finally:
                if event in self._waiters_send:
                    self._waiters_send.remove(event)
        
        async def _wait_for_data(self):
            """Wait for data in channel (async)"""
            import asyncio
            # Check if closed before waiting
            if self._closed:
                return
            event = asyncio.Event()
            self._waiters_recv.append(event)
            # Check again after adding to waiters (race condition fix)
            if self._closed:
                if event in self._waiters_recv:
                    self._waiters_recv.remove(event)
                return
            try:
                await event.wait()
            finally:
                if event in self._waiters_recv:
                    self._waiters_recv.remove(event)
        
        def _wake_send_waiters(self):
            """Wake up waiting senders"""
            import asyncio
            for event in self._waiters_send[:]:
                event.set()
            self._waiters_send.clear()
        
        def _wake_recv_waiters(self):
            """Wake up waiting receivers"""
            import asyncio
            for event in self._waiters_recv[:]:
                event.set()
            self._waiters_recv.clear()
        
        def send_nowait(self, value):
            """Send value without blocking"""
            if self._closed:
                return False
            if len(self._queue) >= self._capacity and self._capacity > 0:
                return False
            self._queue.append(value)
            # Wake up a waiting receiver
            if self._waiters_recv:
                self._wake_recv_waiters()
            return True
        
        def recv_nowait(self):
            """Receive value without blocking"""
            if not self._queue:
                return None
            value = self._queue.pop(0)
            # Wake up a waiting sender
            if self._waiters_send:
                self._wake_send_waiters()
            return value
        
        def close(self):
            """Close the channel"""
            self._closed = True
            # Wake up all waiters
            self._wake_send_waiters()
            self._wake_recv_waiters()
        
        def __len__(self):
            return len(self._queue)
        
        def __bool__(self):
            return not self._closed
    
    class WaitGroup:
        """Pure Python WaitGroup implementation"""
        def __init__(self):
            self._counter = 0
            self._lock = threading.Lock()
            self._condition = threading.Condition(self._lock)
        
        def add(self, delta=1):
            with self._lock:
                self._counter += delta
                if self._counter < 0:
                    raise RuntimeError("WaitGroup counter would be negative")
        
        def done(self):
            with self._lock:
                self._counter -= 1
                if self._counter <= 0:
                    self._condition.notify_all()
        
        async def wait(self):
            with self._lock:
                while self._counter > 0:
                    self._condition.wait()
        
        @property
        def counter(self):
            with self._lock:
                return self._counter
    
    def init_scheduler(num_workers=0, max_fibers=1000000, work_stealing=True):
        """Pure Python scheduler init (no-op)"""
        pass
    
    def shutdown_scheduler(wait=True):
        """Pure Python scheduler shutdown (no-op)"""
        pass
    
    def get_scheduler_stats():
        """Pure Python scheduler stats"""
        return {
            'total_fibers_created': 0,
            'total_fibers_completed': 0,
            'total_context_switches': 0,
            'total_work_steals': 0,
            'current_active_fibers': 0,
            'current_ready_fibers': 0,
        }
    
    def spawn(func, *args):
        """Pure Python spawn using threading"""
        t = threading.Thread(target=func, args=args)
        t.start()
        return t

    def spawn_direct(func, args):
        """Pure Python spawn_direct - call function directly and return result"""
        return func(*args)

    def sleep_ms(ms):
        """Sleep for milliseconds"""
        import time
        time.sleep(ms / 1000.0)

    def sleep_ns(ns):
        """Sleep for nanoseconds"""
        import time
        time.sleep(ns / 1000000000.0)

    def sleep_us(us):
        """Sleep for microseconds"""
        import time
        time.sleep(us / 1000000.0)
    
    def current_fiber_id():
        """Get current thread ID (fiber ID in pure Python)"""
        return threading.current_thread().ident

    def yield_execution():
        """Yield execution (no-op in pure Python)"""
        pass

    def num_workers():
        """Get number of CPU cores"""
        import os
        return os.cpu_count() or 1
    
    # Worker management (pure Python - no-op implementations)
    def check_worker_scaling():
        """Check if worker scaling is needed (no-op in pure Python)"""
        pass
    
    def set_auto_scaling(enabled):
        """Enable/disable auto-scaling (no-op in pure Python)"""
        pass
    
    def set_energy_efficient_mode(enabled):
        """Enable energy-efficient mode (no-op in pure Python)"""
        pass
    
    def get_worker_utilization():
        """Get worker utilization (returns 0 in pure Python)"""
        return 0.0
    
    def get_recommended_workers():
        """Get recommended workers (returns CPU count in pure Python)"""
        import os
        return os.cpu_count() or 1

    # sync/sync_timeout for pure Python fallback
    def sync():
        """Wait for all tasks to complete (pure Python - uses atomic counter)"""
        import time
        while not atomic_all_tasks_complete():
            time.sleep(0.001)

    def sync_timeout(timeout: float) -> bool:
        """Wait for all tasks with a timeout (pure Python)"""
        import time
        deadline = time.time() + timeout
        while not atomic_all_tasks_complete() and time.time() < deadline:
            time.sleep(0.001)
        return atomic_all_tasks_complete()

    def task_count() -> int:
        """Get the number of active tasks (pure Python)"""
        return atomic_task_count()

    def task_completed_count() -> int:
        """Get the number of completed tasks (pure Python)"""
        return 0

    def run(func, *args, **kwargs):
        """Run a function in gsyncio runtime (pure Python - direct call)"""
        return func(*args, **kwargs)

    def _get_task_registry():
        """Get task registry (pure Python - returns None)"""
        return None
    
    # Batch spawn (pure Python fallback)
    def spawn_batch(funcs_and_args):
        """Spawn multiple tasks in batch (pure Python - uses threading)"""
        import threading
        results = []
        for func, args in funcs_and_args:
            t = threading.Thread(target=func, args=args, daemon=True)
            t.start()
            results.append(id(t))  # Return thread ID as fiber ID substitute
        return results
    
    def spawn_batch_fast(funcs_and_args):
        """Ultra-fast batch spawn (pure Python - no return values)"""
        import threading
        for func, args in funcs_and_args:
            t = threading.Thread(target=func, args=args, daemon=True)
            t.start()

    def spawn_batch_ultra_fast(funcs_and_args, store_fiber_ids=0):
        """Ultra-fast batch spawn with GIL release (pure Python fallback)"""
        import threading
        for func, args in funcs_and_args:
            t = threading.Thread(target=func, args=args, daemon=True)
            t.start()
        return len(funcs_and_args)

    # Lock-free atomic operations (pure Python fallback - uses simple counter)
    _atomic_counter = 0
    _atomic_lock = threading.Lock()
    
    def atomic_task_count():
        """Get current task count (pure Python - uses lock)"""
        with _atomic_lock:
            return _atomic_counter
    
    def atomic_inc_task_count():
        """Atomically increment task count (pure Python - uses lock)"""
        global _atomic_counter
        with _atomic_lock:
            _atomic_counter += 1
            return _atomic_counter
    
    def atomic_dec_task_count():
        """Atomically decrement task count (pure Python - uses lock)"""
        global _atomic_counter
        with _atomic_lock:
            _atomic_counter -= 1
            return _atomic_counter

    def atomic_all_tasks_complete():
        """Check if all tasks are complete (pure Python - uses lock)"""
        with _atomic_lock:
            return _atomic_counter == 0


__all__ = [
    'Future',
    'Channel',
    'WaitGroup',
    'init_scheduler',
    'shutdown_scheduler',
    'get_scheduler_stats',
    'spawn',
    'spawn_direct',
    'spawn_batch',
    'spawn_batch_fast',
    'spawn_batch_ultra_fast',
    'sleep_ns',
    'sleep_us',
    'sleep_ms',
    'current_fiber_id',
    'yield_execution',
    'num_workers',
    'sync',
    'sync_timeout',
    'task_count',
    'task_completed_count',
    'run',
    '_HAS_CYTHON',
]
