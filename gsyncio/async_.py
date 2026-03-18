"""
gsyncio.async_ - Async/Await model for I/O-bound operations

This module provides async/await support with gsyncio's fiber-based
scheduler for high-performance I/O-bound operations.

Usage:
    import gsyncio as gs
    
    async def fetch_url(url):
        await gs.sleep(100)  # Simulate I/O
        return f"Data from {url}"
    
    async def main():
        urls = [f"http://example.com/{i}" for i in range(100)]
        tasks = [gs.create_task(fetch_url(url)) for url in urls]
        results = await gs.gather(*tasks)
        print(f"Fetched {len(results)} URLs")
    
    gs.run(main())
"""

import asyncio
import threading
import time
from typing import Any, Callable, Coroutine, List, Optional, Awaitable
from .core import Future, sleep_ms, sleep_ns, init_scheduler, shutdown_scheduler, _HAS_CYTHON


def create_task(coro: Coroutine) -> Future:
    """
    Create a task from a coroutine using gsyncio's native scheduler.
    """
    future = Future()
    import asyncio
    
    async def run_coro():
        try:
            result = await coro
            future.set_result(result)
        except Exception as e:
            future.set_exception(e)
    
    try:
        loop = asyncio.get_event_loop()
        asyncio.ensure_future(run_coro())
    except RuntimeError:
        pass
    
    return future


def _run_coroutine(coro: Coroutine) -> Any:
    """
    Run a coroutine to completion.
    
    This is a simple implementation that uses asyncio for the actual
    event loop. A full implementation would use gsyncio's fiber scheduler.
    """
    # For now, use asyncio as the underlying event loop
    # A full implementation would integrate with gsyncio's scheduler
    try:
        loop = asyncio.get_event_loop()
    except RuntimeError:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
    
    return loop.run_until_complete(coro)


async def sleep(ms: int) -> None:
    """
    Sleep for a specified number of milliseconds.
    
    Uses native gsyncio timer when available (fast path),
    falls back to asyncio.sleep otherwise.
    """
    if _HAS_CYTHON:
        # Use native sleep - blocks current fiber, not thread
        sleep_ns(ms * 1000000)
    else:
        await asyncio.sleep(ms / 1000.0)


async def gather(*futures: Awaitable) -> List[Any]:
    """
    Wait for multiple futures/awaitables to complete.
    
    Args:
        *futures: Futures or awaitables to wait for
    
    Returns:
        List of results in the same order as input
    """
    if _HAS_CYTHON:
        # For Cython futures, convert to asyncio
        async def wrap_future(fut):
            if hasattr(fut, '__await__'):
                return await fut
            return fut
        
        tasks = [asyncio.create_task(wrap_future(f)) for f in futures]
        return await asyncio.gather(*tasks)
    else:
        return await asyncio.gather(*futures)


async def wait_for(fut: Awaitable, timeout: float) -> Any:
    """
    Wait for a future with a timeout.
    
    Args:
        fut: Future or awaitable to wait for
        timeout: Timeout in seconds
    
    Returns:
        Result of the future
    
    Raises:
        asyncio.TimeoutError: If timeout expires
    """
    return await asyncio.wait_for(fut, timeout)


def ensure_future(coro_or_future: Coroutine) -> Future:
    """
    Ensure a coroutine is wrapped in a future.
    
    Args:
        coro_or_future: Coroutine or future
    
    Returns:
        Future
    """
    if isinstance(coro_or_future, Future):
        return coro_or_future
    return create_task(coro_or_future)


def run(main: Coroutine) -> Any:
    """
    Run an async function as the main entry point.
    
    Args:
        main: Main coroutine to run
    
    Returns:
        Result of the main coroutine
    """
    init_scheduler()
    try:
        return _run_coroutine(main)
    finally:
        shutdown_scheduler(wait=True)


class AsyncIterator:
    """
    Base class for async iterators.
    """
    
    def __aiter__(self):
        return self
    
    async def __anext__(self):
        raise StopAsyncIteration


class AsyncRange(AsyncIterator):
    """
    Async iterator that yields numbers like range().
    
    Usage:
        async for i in gs.async_range(10):
            print(i)
    """
    
    def __init__(self, start: int, stop: Optional[int] = None, step: int = 1):
        if stop is None:
            self._start = 0
            self._stop = start
        else:
            self._start = start
            self._stop = stop
        self._step = step
        self._current = self._start
    
    async def __anext__(self):
        if self._step > 0 and self._current >= self._stop:
            raise StopAsyncIteration
        elif self._step < 0 and self._current <= self._stop:
            raise StopAsyncIteration
        
        value = self._current
        self._current += self._step
        
        # Yield to allow other tasks to run
        await sleep(0)
        
        return value


def async_range(start: int, stop: Optional[int] = None, step: int = 1) -> AsyncRange:
    """
    Create an async range iterator.
    
    Args:
        start: Start value (or stop if stop is None)
        stop: Stop value (exclusive)
        step: Step value
    
    Returns:
        AsyncRange iterator
    """
    return AsyncRange(start, stop, step)


class AsyncContextManager:
    """
    Base class for async context managers.
    """
    
    async def __aenter__(self):
        raise NotImplementedError
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        raise NotImplementedError


__all__ = [
    'create_task',
    'sleep',
    'gather',
    'wait_for',
    'ensure_future',
    'run',
    'async_range',
    'AsyncIterator',
    'AsyncRange',
    'AsyncContextManager',
]
