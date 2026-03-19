"""
gsyncio.task - Task/Sync model for fire-and-forget parallelism

Optimized implementation with:
- Lock-free task counting using C11 atomics (all counting done in C!)
- Object pooling for reduced allocation overhead
- Batch spawning for bulk operations
"""

from typing import Callable, Any, List
import inspect
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
    atomic_all_tasks_complete as _atomic_all_tasks_complete,
)
from .async_ import _run_coroutine

# Lock-free task counting using C11 atomics
# All counting is done in C - Python just reads the count!
_tasks_lock = _threading.Lock()  # Only for event setting synchronization
_pending_count = 0  # Legacy, not used
_all_done_event = _threading.Event()
_scheduler_initialized = False


def _task_completion_wrapper(func, args, kwargs):
    """Wrapper that handles task completion tracking.

    Note: Task counting is done entirely in C (scheduler_spawn/scheduler_completion).
    This wrapper just executes the function. Handles both sync and async functions.
    """
    # Check if func is actually a coroutine (already created)
    if inspect.iscoroutine(func):
        # It's already a coroutine, run it
        _run_coroutine(func)
    elif callable(func):
        # Call the function and check if it returns a coroutine
        result = func(*args, **kwargs)
        if inspect.iscoroutine(result):
            # It's an async function, run the coroutine
            _run_coroutine(result)
    # Otherwise it's a sync function, already executed by func(*args, **kwargs)


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
    """Spawn a new task - optimized with lock-free counting in C.

    Performance optimizations:
    - Lock-free atomic counting in C (NO Python overhead!)
    - Lock-free atomic decrement on completion (in C)
    - Minimal wrapper overhead
    - Auto-initializes scheduler on first call
    
    Also supports async functions - they will be run to completion.
    """
    global _scheduler_initialized

    # Ensure scheduler is initialized (lazy init)
    if not _scheduler_initialized:
        _ensure_scheduler()

    # Clear event if count is 0 before spawning (C will increment)
    if _atomic_task_count() == 0:
        _all_done_event.clear()

    # Spawn with module-level wrapper (avoids closure issues)
    # Note: scheduler_spawn() in C does the atomic increment
    # and scheduler completion does the atomic decrement
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
        - Lock-free atomic counting in C (NO Python overhead!)
        - Object pooling reduces allocation overhead
        - Round-robin distribution to workers
    """
    global _scheduler_initialized

    # Ensure scheduler is initialized
    if not _scheduler_initialized:
        _ensure_scheduler()

    # Clear event if count is 0 before spawning batch (C will increment)
    if _atomic_task_count() == 0:
        _all_done_event.clear()

    # Prepare batch - wrapper just executes function (counting is in C)
    batch = [(_task_completion_wrapper, (f, a, {})) for f, a in funcs_and_args]

    # Spawn all tasks (C does atomic increments)
    return _spawn_batch(batch)


def sync():
    """Wait for all spawned tasks to complete (lock-free, C counting)."""
    # Poll atomic count from C and wait on event
    while not _atomic_all_tasks_complete():
        _all_done_event.wait(timeout=0.001)


def sync_timeout(timeout: float) -> bool:
    """Wait for all tasks with a timeout (lock-free, C counting)."""
    import time
    deadline = time.time() + timeout
    while not _atomic_all_tasks_complete() and time.time() < deadline:
        remaining = deadline - time.time()
        _all_done_event.wait(timeout=min(0.001, remaining))
    return _atomic_all_tasks_complete()


def task_count() -> int:
    """Get the number of active tasks (lock-free, read from C)."""
    return _atomic_task_count()


def run(func: Callable, *args, **kwargs) -> Any:
    """Run a function in the gsyncio runtime.
    
    This function handles both regular functions and async functions.
    For async functions, it uses asyncio to run the coroutine.
    
    Args:
        func: A function, coroutine, or async function to run
        *args: Positional arguments to pass to the function
        **kwargs: Keyword arguments to pass to the function
    
    Returns:
        The result of the function execution
    """
    global _scheduler_initialized
    
    # Check if func is actually a coroutine (already created by calling an async function)
    if inspect.iscoroutine(func):
        # It's already a coroutine, run it directly
        coro = func
    elif callable(func):
        # It's a callable, call it to get the result/coroutine
        result = func(*args, **kwargs)
        if inspect.iscoroutine(result):
            # It's an async function, get the coroutine
            coro = result
        else:
            # Regular function result
            return result
    else:
        raise TypeError(f"func must be a callable or coroutine, got {type(func)}")
    
    # Check if scheduler needs initialization
    need_init = not _scheduler_initialized
    
    if need_init:
        _init_scheduler(num_workers=_num_workers())
        _scheduler_initialized = True
    
    try:
        return _run_coroutine(coro)
    finally:
        if need_init:
            _shutdown_scheduler(wait=True)
            _scheduler_initialized = False


def task_fast(func, *args):
    """Fast task spawn without counting (for internal use)."""
    global _scheduler_initialized

    if not _scheduler_initialized:
        _ensure_scheduler()

    return _spawn(func, *args)


def task_batch_fast(funcs_and_args):
    """Fast batch spawn without counting (for internal use)."""
    global _scheduler_initialized

    if not _scheduler_initialized:
        _ensure_scheduler()

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
]
