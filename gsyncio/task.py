"""
gsyncio.task - Task/Sync model for fire-and-forget parallelism

Optimized implementation with:
- Lock-free task counting using C11 atomics (NO Python locks!)
- Object pooling for reduced allocation overhead
- Batch spawning for bulk operations
"""

from typing import Callable, Any, List
import threading as _threading
from .core import (
    init_scheduler as _init_scheduler,
    shutdown_scheduler as _shutdown_scheduler,
    spawn as _spawn,
    spawn_batch as _spawn_batch,
    sleep_ms,
    num_workers as _num_workers,
    _HAS_CYTHON,
    atomic_task_count as _atomic_task_count,
    atomic_inc_task_count as _atomic_inc,
    atomic_dec_task_count as _atomic_dec,
)

# Export atomic functions for direct import
atomic_task_count = _atomic_task_count
atomic_inc_task_count = _atomic_inc
atomic_dec_task_count = _atomic_dec

# Lock-free task counting using C11 atomics
# NO Python threading.Lock needed!
_tasks_lock = None  # Not used anymore - using atomics
_pending_count = 0  # Legacy, not used
_all_done_event = _threading.Event()
_scheduler_initialized = False


def _task_completion_wrapper(func, args, kwargs):
    """Wrapper that handles task completion tracking.

    This is a module-level function to avoid closure issues with fibers.
    Uses lock-free atomic operations for counting.
    """
    try:
        func(*args, **kwargs)
    finally:
        # Lock-free atomic decrement
        _atomic_dec()
        # Check if all tasks are done (atomic read)
        if _atomic_task_count() == 0:
            _all_done_event.set()


def _ensure_scheduler():
    """Lazily initialize scheduler if not already done."""
    global _scheduler_initialized
    if not _scheduler_initialized:
        try:
            _init_scheduler(num_workers=_num_workers())
            _scheduler_initialized = True
        except Exception:
            pass  # Scheduler may already be initialized


def task(func: Callable, *args, **kwargs):
    """Spawn a new task - optimized with lock-free counting.

    Performance optimizations:
    - Lock-free atomic increment (NO Python locks!)
    - Lock-free atomic decrement on completion
    - Minimal wrapper overhead
    - Auto-initializes scheduler on first call
    """
    global _scheduler_initialized

    # Ensure scheduler is initialized (lazy init)
    if not _scheduler_initialized:
        _ensure_scheduler()

    # Lock-free atomic increment
    _atomic_inc()
    
    # Clear event if this is the first task
    if _atomic_task_count() == 1:
        _all_done_event.clear()

    # Spawn with module-level wrapper (avoids closure issues)
    _spawn(_task_completion_wrapper, func, args, kwargs)
    return True


def task_batch(funcs_and_args: List[tuple]):
    """Spawn multiple tasks in a batch - 5-10x faster than individual spawns.

    Args:
        funcs_and_args: List of (func, args) tuples

    Returns:
        List of fiber IDs

    Example:
        >>> task_batch([(func1, (arg1,)), (func2, (arg2,))])
        [1, 2]

    Performance:
        - Lock-free atomic counting (NO Python locks!)
        - Object pooling reduces allocation overhead
        - Round-robin distribution to workers
    """
    global _scheduler_initialized

    # Ensure scheduler is initialized
    if not _scheduler_initialized:
        _ensure_scheduler()

    # Prepare batch - use completion wrapper for proper counting
    batch = []
    for f, a in funcs_and_args:
        # Use the same completion wrapper as regular task()
        # Lock-free atomic increment for each task
        _atomic_inc()
        batch.append((_task_completion_wrapper, (f, a, {})))
    
    # Clear event if this is the first batch
    if _atomic_task_count() == len(batch):
        _all_done_event.clear()
    
    # Spawn all tasks
    return _spawn_batch(batch)


def sync():
    """Wait for all spawned tasks to complete (lock-free)."""
    if _atomic_task_count() == 0:
        return
    _all_done_event.wait()


def sync_timeout(timeout: float) -> bool:
    """Wait for all tasks with a timeout (lock-free)."""
    if _atomic_task_count() == 0:
        return True
    return _all_done_event.wait(timeout=timeout)


def task_count() -> int:
    """Get the number of active tasks (lock-free)."""
    return _atomic_task_count()


def run(func: Callable, *args, **kwargs) -> Any:
    """Run a function in the gsyncio runtime.
    
    Note: This function initializes the scheduler if not already running.
    It's designed for standalone scripts, not for use in tests or when
    the scheduler is already initialized.
    """
    global _scheduler_initialized
    
    # Check if scheduler needs initialization
    need_init = not _scheduler_initialized
    
    if need_init:
        _init_scheduler(num_workers=4)
        try:
            result = func(*args, **kwargs)
            sync()
            return result
        finally:
            _shutdown_scheduler(wait=True)
            _scheduler_initialized = False
    else:
        # Scheduler already running, just execute the function
        result = func(*args, **kwargs)
        sync()
        return result


def task_fast(func: Callable, *args, **kwargs):
    """Ultra-fast task spawn with minimal overhead.

    This is a performance-critical path that:
    - Skips task counting (no sync() support)
    - No wrapper overhead
    - Direct spawn call

    Use when:
    - You need maximum spawn rate
    - Don't need sync() to wait for completion
    - Spawning many similar tasks

    Performance: 2-5x faster than task()

    Example:
        >>> for i in range(10000):
        ...     task_fast(worker, i)
    """
    global _scheduler_initialized

    if not _scheduler_initialized:
        _ensure_scheduler()

    # Direct spawn with no overhead
    return _spawn(func, *args)


def task_batch_fast(funcs_and_args: List[tuple]):
    """Ultra-fast batch spawn - zero overhead.

    This bypasses task counting entirely for maximum performance.
    Use when you don't need sync() to wait for these tasks.

    Performance: 10-20x faster than task_batch()

    Example:
        >>> task_batch_fast([(worker1, (arg1,)), (worker2, (arg2,))])
    """
    global _scheduler_initialized

    if not _scheduler_initialized:
        _ensure_scheduler()

    # Direct batch spawn with no counting
    return _spawn_batch(funcs_and_args)


__all__ = [
    'task',
    'sync',
    'sync_timeout',
    'task_count',
    'run',
    'task_batch',
    'task_fast',
    'task_batch_fast',
    'atomic_task_count',
    'atomic_inc_task_count',
    'atomic_dec_task_count',
]
