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
    spawn_direct as _spawn_direct,
    spawn_batch as _spawn_batch,
    sleep_ms,
    num_workers as _num_workers,
    _HAS_CYTHON,
    atomic_task_count as _atomic_task_count,
    atomic_all_tasks_complete as _atomic_all_tasks_complete,
    sync as _c_sync,
    sync_timeout as _c_sync_timeout,
    _get_task_registry as _get_task_registry,
)
from .async_ import _run_coroutine

# Lock-free task counting using C11 atomics
# All counting is done in C - Python just reads the count!
# NOTE: _tasks_lock removed - was unused and caused overhead
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


def task_with_wrapper(func: Callable, *args, **kwargs):
    """Spawn a new task with Python wrapper - for async function support.

    This is the legacy task() function that includes:
    - _task_completion_wrapper for completion tracking
    - inspect.iscoroutine() checks for async function support
    
    Note: This has more overhead than task_direct(). Use task_direct() for
    maximum performance with sync-only functions.
    
    For new code, prefer using task() which auto-batches for better performance.
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


def task(func, *args, **kwargs):
    """Spawn task(s) - optimized default for fire-and-forget parallelism.

    This function automatically handles both single tasks and batch spawning:
    - For a single function call: spawns one task (uses batch path internally)
    - For multiple tasks: uses batch spawning for 5-10x better performance

    Performance:
        - Lock-free atomic counting in C (NO Python overhead!)
        - Object pooling reduces allocation overhead
        - Round-robin distribution to workers
        - Auto-initializes scheduler on first call

    Also supports async functions - they will be run to completion.

    Args:
        func: Either:
            - A callable to spawn (single task)
            - A list of (func, args) tuples for batch spawning
        *args: Arguments to pass to func (if single callable)
        **kwargs: Keyword arguments (for single callable)

    Returns:
        For single task: True
        For batch: List of fiber IDs

    Example (single task):
        >>> task(worker, 1000)
        True

    Example (batch):
        >>> task([(func1, (arg1,)), (func2, (arg2,))])
        [1, 2]
    """
    global _scheduler_initialized

    # Ensure scheduler is initialized
    if not _scheduler_initialized:
        _ensure_scheduler()

    # Get task registry
    registry = _get_task_registry()

    # Check if this is a batch call (list of tuples) or single task
    # Single task: task(worker, 1000) -> func=worker, args=(1000,)
    # Batch call: task([(func, (args,)), ...]) -> func=list
    if isinstance(func, list) and len(func) > 0:
        # Batch spawning via TaskRegistry (proper counting)
        return registry.spawn_batch([(f, a) for f, a in func])
    else:
        # Single task via TaskRegistry (proper counting)
        registry.spawn(func, *args, **kwargs)
        return True


def task_batch(funcs_and_args: List[tuple]):
    """Spawn multiple tasks in a batch - 5-10x faster than individual spawns.

    Note: This is now an alias for task() when called with a list.
    Prefer using task([(func, args), ...]) for the same functionality.

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
    """Wait for all spawned tasks to complete (uses C pthread_cond_wait)."""
    # Use C sync which properly releases GIL via pthread_cond_wait
    if _HAS_CYTHON:
        _c_sync()
    else:
        # Fallback for pure Python
        import select
        while not _atomic_all_tasks_complete():
            select.select([], [], [], 0.001)


def sync_timeout(timeout: float) -> bool:
    """Wait for all tasks with a timeout (lock-free, C counting)."""
    import time
    import select
    deadline = time.time() + timeout
    while not _atomic_all_tasks_complete() and time.time() < deadline:
        remaining = deadline - time.time()
        select.select([], [], [], min(0.001, remaining))
    return _atomic_all_tasks_complete()


def task_count() -> int:
    """Get the number of active tasks (lock-free, read from C)."""
    return _atomic_task_count()


def run(func: Callable, *args, mapping: str = "native", **kwargs) -> Any:
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
        mode_val = 1 if mapping.lower() == "hybrid" else 0
        _init_scheduler(num_workers=_num_workers(), stack_mode=mode_val, max_fibers=100_000_000)
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


def task_direct(func, *args):
    """Ultra-fast task spawn - bypasses Python wrapper entirely.
    
    This is the fastest way to spawn a single task:
    1. No _task_completion_wrapper overhead
    2. No inspect.iscoroutine() checks
    3. No tuple allocation for kwargs
    4. Direct call to scheduler_spawn in C
    
    Performance:
        - Expected 5-10x faster than task() for simple functions
        - Saves ~50µs per task due to eliminated wrapper overhead
    
    Limitations:
        - Does NOT support async functions (use task() for those)
        - Does NOT support kwargs (use task() for those)
        - Does NOT track completion (use sync() to wait for ALL tasks)
    
    Args:
        func: A callable (NOT an async function)
        *args: Arguments to pass to the callable
    
    Returns:
        True if task was spawned successfully
    
    Example:
        >>> def worker(n):
        ...     print(f"Working on {n}")
        >>> task_direct(worker, 42)
        >>> sync()  # Wait for all tasks including task_direct
    """
    global _scheduler_initialized

    # Ensure scheduler is initialized (lazy init)
    if not _scheduler_initialized:
        _ensure_scheduler()

    # Clear event if count is 0 before spawning (C will increment)
    if _atomic_task_count() == 0:
        _all_done_event.clear()

    # Direct spawn - bypasses _task_completion_wrapper entirely!
    # This is the key optimization: no wrapper, no iscoroutine checks
    _spawn_direct(func, args)
    return True


def task_batch_fast(funcs_and_args):
    """Fast batch spawn without counting (for internal use)."""
    global _scheduler_initialized

    if not _scheduler_initialized:
        _ensure_scheduler()

    return _spawn_batch(funcs_and_args)


__all__ = [
    'task',
    'task_with_wrapper',
    'task_direct',
    'sync',
    'sync_timeout',
    'task_count',
    'run',
    'task_batch',
    'task_fast',
    'task_batch_fast',
]
