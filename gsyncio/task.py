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
import weakref

# Global task tracking using WeakSet (lock-free)
_active_tasks = weakref.WeakSet()


def task(func: Callable, *args, **kwargs) -> threading.Thread:
    """
    Spawn a new task (fire-and-forget parallel work).
    
    Args:
        func: Function to execute
        *args: Arguments to pass to function
        **kwargs: Keyword arguments to pass to function
    
    Returns:
        Thread object representing the task
    """
    def wrapper():
        try:
            func(*args, **kwargs)
        finally:
            pass  # WeakSet handles cleanup automatically
    
    t = spawn(wrapper)
    
    # WeakSet.add() is thread-safe and lock-free
    _active_tasks.add(t)
    
    return t


def sync():
    """
    Wait for all spawned tasks to complete.
    
    This blocks until all tasks created with gs.task() have finished.
    """
    # Get snapshot of current tasks
    tasks = list(_active_tasks)
    
    # Join all tasks
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
        tasks = list(_active_tasks)
        
        if not tasks:
            return True
        
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        
        # Join one task with remaining timeout
        for t in tasks:
            if hasattr(t, 'join'):
                t.join(timeout=min(remaining, 0.01))  # Small timeout for responsiveness
            break  # Check for more tasks
        
        if time.time() >= deadline:
            return len(list(_active_tasks)) == 0


def task_count() -> int:
    """
    Get the number of active tasks.
    
    Returns:
        Number of currently running tasks
    """
    # WeakSet can be converted to list without lock
    return len(list(_active_tasks))


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
