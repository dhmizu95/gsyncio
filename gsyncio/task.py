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
import threading as _threading

_active_tasks = []
_tasks_lock = _threading.Lock()


def task(func: Callable, *args, **kwargs):
    """
    Spawn a new task (fire-and-forget parallel work).
    
    Args:
        func: Function to execute
        *args: Arguments to pass to function
        **kwargs: Keyword arguments to pass to function
    
    Returns:
        Handle to the spawned task (FiberHandle or Thread)
    """
    def wrapper():
        func(*args, **kwargs)
    
    t = spawn(wrapper)
    
    with _tasks_lock:
        _active_tasks.append(t)
    
    return t


def sync():
    """
    Wait for all spawned tasks to complete.
    
    This blocks until all tasks created with gs.task() have finished.
    """
    with _tasks_lock:
        tasks = list(_active_tasks)
        _active_tasks.clear()
    
    for t in tasks:
        if hasattr(t, 'join'):
            t.join()


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
        with _tasks_lock:
            tasks = list(_active_tasks)
        
        if not tasks:
            return True
        
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        
        for t in tasks:
            if hasattr(t, 'join'):
                t.join(timeout=min(remaining, 0.01))
            break
        
        if time.time() >= deadline:
            with _tasks_lock:
                return len(_active_tasks) == 0


def task_count() -> int:
    """
    Get the number of active tasks.
    
    Returns:
        Number of currently running tasks
    """
    with _tasks_lock:
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
