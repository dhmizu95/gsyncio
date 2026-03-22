"""
gsyncio.core - Python wrappers for native C fiber scheduler
"""

from ._gsyncio_core import (
    Fiber as _CFiber,
    Future as _CFuture,
    EventLoop as _CEventLoop,
    fiber_create as _fiber_create,
    loop_create as _loop_create,
    loop_run as _loop_run,
    future_create as _future_create,
)
import threading

# Thread-local storage for current loop
_local = threading.local()

def get_current_loop():
    """Get current thread's event loop."""
    return getattr(_local, 'loop', None)

def set_current_loop(loop):
    """Set current thread's event loop."""
    _local.loop = loop

class Fiber:
    """Python wrapper for C fiber."""
    def __init__(self, coro):
        self._fiber = _fiber_create(coro)
    
    def resume(self):
        """Resume fiber execution."""
        from ._gsyncio_core import fiber_resume
        fiber_resume(self._fiber)

class Task(Fiber):
    """A Fiber that is automatically scheduled."""
    def __init__(self, coro, loop=None):
        super().__init__(coro)
        if loop is None:
            loop = get_current_loop()
        if loop:
            from ._gsyncio_core import loop_add_ready
            loop_add_ready(loop._loop, self._fiber)

def create_task(coro, loop=None):
    """Create and schedule a task."""
    return Task(coro, loop)
    """Python wrapper for C event loop."""
    def __init__(self):
        self._loop = _loop_create()
    
    def run(self):
        """Run event loop until completion."""
        set_current_loop(self)
        try:
            _loop_run(self._loop)
        finally:
            set_current_loop(None)
    
    def create_future(self):
        """Create a future."""
        return Future(self._loop)

class Future:
    """Python wrapper for C future."""
    def __init__(self, loop):
        self._future = _future_create()
        self._callbacks = []
    
    def __await__(self):
        """Await the future - parks current fiber."""
        if not self.done():
            yield self._future
        return self.result()
    
    def done(self):
        """Check if future is done."""
        return self._future.done
    
    def result(self):
        """Get future result."""
        return self._future.result

    def set_result(self, result):
        """Set future result."""
        from ._gsyncio_core import future_set_result
        future_set_result(self._future, result)

def fiber_park():
    """Park current fiber."""
    from ._gsyncio_core import fiber_park as _fiber_park
    _fiber_park()

def is_future(obj):
    """Check if object is a future."""
    return isinstance(obj, (Future, _CFuture))

__all__ = ['Fiber', 'Task', 'create_task', 'EventLoop', 'Future', 'get_current_loop', 'set_current_loop', 'fiber_park', 'is_future']
