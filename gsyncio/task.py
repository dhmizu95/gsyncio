"""
gsyncio.task - Task/Sync model for fire-and-forget parallelism
"""

from typing import Callable, Any
import threading as _threading
from .core import (
    init_scheduler as _init_scheduler,
    shutdown_scheduler as _shutdown_scheduler,
    spawn as _spawn,
    sleep_ms,
)

# Active tasks tracking
_tasks_lock = _threading.Lock()
_pending_count = 0
_all_done_event = _threading.Event()


def _reset_task_state():
    """Reset task tracking state"""
    global _pending_count
    with _tasks_lock:
        _pending_count = 0
        _all_done_event.clear()


def task(func: Callable, *args, **kwargs):
    """Spawn a new task."""
    global _pending_count

    def wrapper():
        global _pending_count
        try:
            func(*args, **kwargs)
        except Exception as e:
            import sys
            print(f"Task exception: {e}", file=sys.stderr)
        finally:
            with _tasks_lock:
                _pending_count -= 1
                if _pending_count == 0:
                    _all_done_event.set()

    with _tasks_lock:
        _pending_count += 1
        _all_done_event.clear()

    _spawn(wrapper)
    return True


def sync():
    """Wait for all spawned tasks to complete."""
    with _tasks_lock:
        if _pending_count == 0:
            return
    _all_done_event.wait()


def sync_timeout(timeout: float) -> bool:
    """Wait for all tasks with a timeout."""
    with _tasks_lock:
        if _pending_count == 0:
            return True
    return _all_done_event.wait(timeout=timeout)


def task_count() -> int:
    """Get the number of active tasks."""
    with _tasks_lock:
        return _pending_count


def run(func: Callable, *args, **kwargs) -> Any:
    """Run a function in the gsyncio runtime."""
    _init_scheduler(num_workers=4)
    _reset_task_state()
    try:
        result = func(*args, **kwargs)
        sync()
        return result
    finally:
        _shutdown_scheduler(wait=True)
        _reset_task_state()


__all__ = ['task', 'sync', 'sync_timeout', 'task_count', 'run']
