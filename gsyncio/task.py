"""
gsyncio.task - Task/Sync model for fire-and-forget parallelism

This module provides the task/sync model for spawning concurrent
tasks that run in parallel and can be waited on collectively.

Usage:
    import gsyncio as gs
    
    def worker(n):
        result = sum(range(n))
        print(f"Worker completed: {result}")
    
    def main():
        # Spawn multiple tasks
        for i in range(10):
            gs.task(worker, 1000)
        
        # Wait for all tasks to complete
        gs.sync()
        print("All tasks completed")
    
    gs.run(main)
"""

import threading
from typing import Callable, Any, List
from .core import spawn, yield_execution, init_scheduler, shutdown_scheduler

# Global task tracking - use a regular list with lock for FiberHandle support
_active_tasks = []
_active_tasks_lock = threading.Lock()


def task(func: Callable, *args, **kwargs):
    """
    Spawn a new task (fire-and-forget parallel work).
    """
    # Spawn and track the task handle
    handle = spawn(func, *args, **kwargs)
    with _active_tasks_lock:
        _active_tasks.append(handle)
    return handle


def sync():
    """
    Wait for all spawned tasks to complete.
    """
    # Get copy of current tasks under lock
    with _active_tasks_lock:
        tasks = list(_active_tasks)
    
    # Wait for each task to complete
    for t in tasks:
        if hasattr(t, 'join'):
            t.join()
        elif hasattr(t, 'join'):
            # Handle FiberHandle with join method
            t.join()
    
    # Clean up completed tasks
    with _active_tasks_lock:
        _active_tasks[:] = [t for t in _active_tasks if hasattr(t, 'is_alive') and t.is_alive()]


def sync_timeout(timeout: float) -> bool:
    """
    Wait for all tasks with a timeout.
    
    Args:
        timeout: Maximum time to wait in seconds
    
    Returns:
        True if all tasks completed, False if timeout occurred
    """
    import time
    
    deadline = time.time() + timeout
    
    while True:
        with _active_tasks_lock:
            tasks = list(_active_tasks)
        
        if not tasks:
            return True
        
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        
        # Join tasks with remaining timeout
        for t in tasks:
            if hasattr(t, 'join'):
                t.join(timeout=min(remaining, 0.01))
            elif hasattr(t, 'wait'):
                # Handle FiberHandle with wait method
                t.wait(timeout=min(remaining, 0.01))
        
        with _active_tasks_lock:
            remaining_tasks = [t for t in _active_tasks if hasattr(t, 'is_alive') and t.is_alive()]
            if not remaining_tasks:
                return True
        
        if time.time() >= deadline:
            return False


def task_count() -> int:
    """
    Get the number of active tasks.
    
    Returns:
        Number of currently running tasks
    """
    with _active_tasks_lock:
        return len(_active_tasks)


def run(func: Callable, *args, **kwargs) -> Any:
    """
    Run a function in the gsyncio runtime.
    
    This initializes the scheduler, runs the function, and shuts down.
    
    Args:
        func: Function to run
        *args: Arguments to pass to function
        **kwargs: Keyword arguments to pass to function
    
    Returns:
        Result of the function
    """
    init_scheduler()
    try:
        return func(*args, **kwargs)
    finally:
        shutdown_scheduler(wait=True)


__all__ = ['task', 'sync', 'sync_timeout', 'task_count', 'run']
