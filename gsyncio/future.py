"""
gsyncio.future - Future implementation for async/await

This module provides Future class for representing values that
will be available in the future (like asyncio.Future).

Usage:
    import gsyncio as gs
    
    async def compute():
        future = gs.Future()
        
        def do_work():
            result = expensive_computation()
            future.set_result(result)
        
        gs.task(do_work)
        
        # Wait for result
        value = await future
        print(f"Result: {value}")
    
    gs.run(compute)
"""

from typing import Any, Optional, Callable, List
from .core import Future as _Future


class Future:
    """
    Future representing a value that will be available later.
    
    A Future is an awaitable object that represents the eventual
    result of an asynchronous operation.
    """
    
    def __init__(self):
        self._future = _Future()
        self._callbacks: List[Callable] = []
        self._done = False
    
    @property
    def done(self) -> bool:
        """Check if the future is done"""
        return self._future.done or self._done
    
    @property
    def cancelled(self) -> bool:
        """Check if the future is cancelled"""
        return self._future.cancelled
    
    def result(self, timeout: Optional[float] = None) -> Any:
        """
        Get the result of the future.
        
        Args:
            timeout: Optional timeout in seconds
        
        Returns:
            Result value
        
        Raises:
            Exception: If future completed with exception
        """
        return self._future.result(timeout)
    
    def exception(self, timeout: Optional[float] = None) -> Optional[Exception]:
        """
        Get the exception of the future.
        
        Args:
            timeout: Optional timeout in seconds
        
        Returns:
            Exception if future failed, None otherwise
        """
        return self._future.exception(timeout)
    
    def set_result(self, result: Any) -> None:
        """
        Set the result of the future.
        
        Args:
            result: Result value
        
        Raises:
            RuntimeError: If future is already done
        """
        self._future.set_result(result)
        self._done = True
        for cb in self._callbacks:
            try:
                cb(self)
            except Exception:
                pass
    
    def set_exception(self, exc: Exception) -> None:
        """
        Set the exception of the future.
        
        Args:
            exc: Exception
        
        Raises:
            RuntimeError: If future is already done
        """
        self._future.set_exception(exc)
        self._done = True
        for cb in self._callbacks:
            try:
                cb(self)
            except Exception:
                pass
    
    def add_callback(self, cb: Callable[['Future'], None]) -> None:
        """
        Add a callback to be called when future is done.
        
        Args:
            cb: Callback function that takes the future as argument
        """
        if self.done:
            cb(self)
        else:
            self._callbacks.append(cb)
    
    def remove_callback(self, cb: Callable[['Future'], None]) -> None:
        """
        Remove a callback.
        
        Args:
            cb: Callback to remove
        """
        if cb in self._callbacks:
            self._callbacks.remove(cb)
    
    def __await__(self):
        """Make future awaitable - supports both sync and async contexts"""
        if not self.done:
            import asyncio
            try:
                # Try to get running loop - if we're in async context
                loop = asyncio.get_running_loop()
                # We're in async context - use asyncio Future to wrap our future
                async_fut = asyncio.Future()

                def on_done(fut):
                    if not async_fut.done():
                        try:
                            result = self._future.result()
                            async_fut.set_result(result)
                        except Exception as e:
                            async_fut.set_exception(e)

                self.add_callback(on_done)
                yield from async_fut
            except RuntimeError:
                # No running event loop or not in asyncio context
                from .core import current_fiber_id, yield_execution
                
                if current_fiber_id() != 0:
                    # We are in a gsyncio fiber context
                    # Use a busy-wait with yield or a park if we had direct access
                    # Since we don't have direct fiber_park here conveniently,
                    # we can use the C future's blocking wait which we just fixed
                    # to release the GIL, so it's safe-ish, but let's try to be better.
                    while not self.done:
                        yield_execution()
                else:
                    # Sync context (standard thread)
                    import threading
                    event = threading.Event()
                    def on_done(fut):
                        event.set()
                    self.add_callback(on_done)
                    event.wait()

        # Return the result (result() will raise any exception)
        return self._future.result()
    
    def __iter__(self):
        """Make future iterable for yield-from syntax"""
        return self.__await__()
    
    def __repr__(self) -> str:
        if self.done:
            try:
                result = self.result()
                return f"<Future done={True} result={result!r}>"
            except Exception as e:
                return f"<Future done={True} exception={e!r}>"
        return f"<Future done={False}>"


def ensure_future(coro_or_future) -> Future:
    """
    Ensure an object is a Future.
    
    If the object is already a Future, return it. Otherwise,
    wrap the coroutine in a Future.
    
    Args:
        coro_or_future: Coroutine or Future
    
    Returns:
        Future
    """
    if isinstance(coro_or_future, Future):
        return coro_or_future
    
    from .async_ import create_task
    return create_task(coro_or_future)


def is_future(obj: Any) -> bool:
    """
    Check if an object is a Future.
    
    Args:
        obj: Object to check
    
    Returns:
        True if object is a Future
    """
    return isinstance(obj, Future)


__all__ = ['Future', 'ensure_future', 'is_future']
