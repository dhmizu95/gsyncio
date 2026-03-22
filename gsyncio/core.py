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
    Channel as _CChannel,
    sleep as _core_sleep,
)
Channel = _CChannel
_HAS_CYTHON = True
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

    def status(self):
        """Get task status."""
        return self._fiber.state

    def result(self):
        """Get task result."""
        return self._fiber.result

    def exception(self):
        """Get task exception."""
        return self._fiber.exception

    def done(self):
        """Check if task is done."""
        from ._gsyncio_core import FIBER_STATE_DONE
        return self._fiber.state == FIBER_STATE_DONE

def create_task(coro):
    """Create and start a task."""
    loop = get_current_loop()
    if not loop:
        raise RuntimeError("No event loop running")
    return Task(coro, loop)

class EventLoop:
    """Python wrapper for C event loop."""
    def __init__(self):
        self._loop = _loop_create()
    
    def run(self):
        """Run event loop until completion."""
        set_current_loop(self)
        try:
            from ._gsyncio_core import loop_run
            loop_run(self._loop)
        finally:
            set_current_loop(None)

    def run_main(self, coro):
        """Run main coroutine."""
        from ._gsyncio_core import loop_set_main_fiber
        f = Fiber(coro)
        loop_set_main_fiber(self._loop, f._fiber)
        self.run()

    def create_task(self, coro):
        """Create and start a task."""
        return Task(coro, self)

def run(coro):
    """Run main coroutine."""
    loop = EventLoop()
    loop.run_main(coro)

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

async def sleep(seconds):
    """Sleep for duration."""
    res = _core_sleep(seconds)
    if is_future(res):
        await res

__all__ = ['run', 'create_task', 'EventLoop', 'sleep', 'Future', 'get_current_loop', 'set_current_loop', 'fiber_park', 'is_future']
