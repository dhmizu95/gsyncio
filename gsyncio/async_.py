"""
gsyncio.async_ - Native async/await with C-based fiber scheduling

This module provides async/await support with gsyncio's fiber-based
scheduler for high-performance I/O-bound operations.
Uses native C implementations for critical paths.

Usage:
    import gsyncio as gs

    async def fetch_url(url):
        await gs.sleep(100)  # Native fiber-aware sleep
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

try:
    from ._gsyncio_core import (
        GSocket,
        gather_native,
        wait_for_native,
        sleep_ns as _sleep_ns,
    )
    _HAS_NATIVE_GATHER = True
except ImportError:
    _HAS_NATIVE_GATHER = False


def create_task(coro: Coroutine) -> Future:
    """Create a task from a coroutine using gsyncio's native scheduler."""
    future = Future()
    
    async def run_coro():
        try:
            result = await coro
            future.set_result(result)
        except Exception as e:
            future.set_exception(e)
    
    try:
        loop = asyncio.get_event_loop()
        asyncio.ensure_future(run_coro())
    except RuntimeError as e:
        import sys
        print(f"Warning: Could not schedule coroutine: {e}", file=sys.stderr)
        future.set_exception(RuntimeError(f"Failed to schedule coroutine: {e}"))
    
    return future


def _run_coroutine(coro: Coroutine) -> Any:
    """Run a coroutine to completion."""
    try:
        loop = asyncio.get_event_loop()
    except RuntimeError:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
    
    return loop.run_until_complete(coro)


async def sleep(ms: int) -> None:
    """Sleep for a specified number of milliseconds.
    
    Uses native gsyncio timer when in fiber context (fast path).
    Falls back to asyncio.sleep when in asyncio context.
    """
    from .core import current_fiber_id
    
    if _HAS_CYTHON and current_fiber_id() != 0:
        # We are on a gsyncio fiber - use native sleep
        sleep_ns(ms * 1000000)
        # Yield to allow other fibers to run (sleep_ns already yields, but this
        # makes the function a coroutine as expected)
        await asyncio.sleep(0)
    else:
        # Fallback to standard asyncio sleep
        await asyncio.sleep(ms / 1000.0)


async def gather(*futures: Awaitable, return_exceptions: bool = False) -> List[Any]:
    """Wait for multiple futures/awaitables to complete.
    
    Uses native C implementation for better performance when available.
    
    Args:
        *futures: Futures or awaitables to wait for
        return_exceptions: If True, exceptions are returned as results
    
    Returns:
        List of results in the same order as input
    """
    if _HAS_NATIVE_GATHER and all(hasattr(f, '__await__') or hasattr(f, 'done') for f in futures):
        return await gather_native(*futures)
    
    results = []
    for fut in futures:
        try:
            if hasattr(fut, '__await__'):
                results.append(await fut)
            else:
                results.append(fut)
        except Exception as e:
            if return_exceptions:
                results.append(e)
            else:
                raise
    return results


async def wait_for(fut: Awaitable, timeout: float) -> Any:
    """Wait for a future with a timeout.
    
    Uses native C implementation when available.
    
    Args:
        fut: Future or awaitable to wait for
        timeout: Timeout in seconds
    
    Returns:
        Result of the future
    
    Raises:
        asyncio.TimeoutError: If timeout expires
    """
    if _HAS_NATIVE_GATHER:
        return await wait_for_native(fut, timeout)
    
    return await asyncio.wait_for(fut, timeout)


def ensure_future(coro_or_future: Coroutine) -> Future:
    """Ensure a coroutine is wrapped in a future."""
    if isinstance(coro_or_future, Future):
        return coro_or_future
    return create_task(coro_or_future)


def run(main: Coroutine) -> Any:
    """Run an async function as the main entry point."""
    init_scheduler()
    try:
        return _run_coroutine(main)
    finally:
        shutdown_scheduler(wait=True)


class AsyncIterator:
    """Base class for async iterators."""
    
    def __aiter__(self):
        return self
    
    async def __anext__(self):
        raise StopAsyncIteration


class AsyncRange(AsyncIterator):
    """Async iterator that yields numbers like range().
    
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
        
        await sleep(0)
        
        return value


def async_range(start: int, stop: Optional[int] = None, step: int = 1) -> AsyncRange:
    """Create an async range iterator."""
    return AsyncRange(start, stop, step)


class AsyncContextManager:
    """Base class for async context managers."""

    async def __aenter__(self):
        raise NotImplementedError

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        raise NotImplementedError


def create_tcp_socket():
    """Create a native TCP socket."""
    if _HAS_CYTHON:
        return GSocket.tcp()
    raise RuntimeError("Native I/O not available")


def create_udp_socket():
    """Create a native UDP socket."""
    if _HAS_CYTHON:
        return GSocket.udp()
    raise RuntimeError("Native I/O not available")


def has_native_io():
    """Check if native I/O is available."""
    return _HAS_CYTHON


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
    'create_tcp_socket',
    'create_udp_socket',
    'has_native_io',
]
