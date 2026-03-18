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

from typing import Callable, Any, List
import threading as _threading
from .core import (
    init_scheduler as _init_scheduler,
    shutdown_scheduler as _shutdown_scheduler,
    spawn as _spawn,
    sleep_ms,
    num_workers,
)

# Active tasks tracking
_active_tasks = []
_tasks_lock = _threading.Lock()
_completion_event = _threading.Event()


def task(func: Callable, *args, **kwargs):
    """
    Spawn a new task (fire-and-forget parallel work).

    Args:
        func: Function to execute
        *args: Arguments to pass to function
        **kwargs: Keyword arguments to pass to function

    Returns:
        Thread handle for the spawned task
    """
    def wrapper():
        try:
            func(*args, **kwargs)
        except Exception as e:
            import sys
            print(f"Task exception: {e}", file=sys.stderr)
        finally:
            # Mark task as complete
            with _tasks_lock:
                if t in _active_tasks:
                    _active_tasks.remove(t)
                if not _active_tasks:
                    _completion_event.set()

    t = _spawn(wrapper)

    with _tasks_lock:
        _active_tasks.append(t)
        _completion_event.clear()

    return t


def sync():
    """
    Wait for all spawned tasks to complete.

    This blocks until all tasks created with gs.task() have finished.
    """
    with _tasks_lock:
        if not _active_tasks:
            return

    # Wait for completion event
    _completion_event.wait()


def sync_timeout(timeout: float) -> bool:
    """
    Wait for all tasks with a timeout.

    Args:
        timeout: Maximum time to wait in seconds

    Returns:
        True if all tasks completed, False if timeout occurred
    """
    with _tasks_lock:
        if not _active_tasks:
            return True

    # Wait for completion event with timeout
    return _completion_event.wait(timeout=timeout)


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
    _init_scheduler(num_workers=num_workers())
    try:
        result = func(*args, **kwargs)
        sync()
        return result
    finally:
        _shutdown_scheduler(wait=True)


__all__ = ['task', 'sync', 'sync_timeout', 'task_count', 'run']
