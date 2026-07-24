"""
gsyncio._async — Async/await helpers.

Works with both the C extension and pure-Python fallback.
"""
import asyncio
import inspect
from typing import Any, Coroutine, List, Optional, Awaitable

try:
    from ._gsyncio_core import GSocket, gather_native, wait_for_native, sleep_ns as _c_sleep_ns
    _HAS_NATIVE = True
except ImportError:
    _HAS_NATIVE = False

from .core import Future, sleep_ms, sleep_ns, init_scheduler, shutdown_scheduler, _HAS_CYTHON


# ── create_task ───────────────────────────────────────────────────────────────
def create_task(coro: Coroutine) -> Future:
    """Wrap a coroutine in a gsyncio Future and schedule it."""
    future = Future()

    async def _run():
        try:
            result = await coro
            future.set_result(result)
        except Exception as e:
            future.set_exception(e)

    try:
        loop = asyncio.get_event_loop()
        asyncio.ensure_future(_run(), loop=loop)
    except RuntimeError:
        # No running loop — run inline
        asyncio.run(_run())

    return future


# ── _run_coroutine ────────────────────────────────────────────────────────────
def _run_coroutine(coro: Coroutine) -> Any:
    """Run a coroutine to completion (creates/reuses event loop)."""
    try:
        loop = asyncio.get_event_loop()
        if loop.is_closed():
            raise RuntimeError("closed")
    except RuntimeError:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
    return loop.run_until_complete(coro)


# ── sleep ─────────────────────────────────────────────────────────────────────
async def sleep(ms: float) -> None:
    """Sleep for *ms* milliseconds (fiber-aware when on a C fiber)."""
    from .core import current_fiber_id
    if _HAS_CYTHON and current_fiber_id() != 0:
        # Running on a native gsyncio fiber — use C sleep
        _c_sleep_ns(int(ms * 1_000_000))
        await asyncio.sleep(0)   # yield control to event loop
    else:
        await asyncio.sleep(ms / 1000.0)


# ── gather ────────────────────────────────────────────────────────────────────
async def gather(*awaitables: Awaitable,
                 return_exceptions: bool = False) -> List[Any]:
    """Concurrently await multiple awaitables."""
    # Prefer asyncio.gather for true concurrency
    tasks = []
    for a in awaitables:
        if asyncio.iscoroutine(a) or asyncio.isfuture(a):
            tasks.append(a)
        elif hasattr(a, '__await__'):
            tasks.append(_wrap_awaitable(a))
        else:
            tasks.append(_immediate(a))

    return list(await asyncio.gather(*tasks, return_exceptions=return_exceptions))


async def _wrap_awaitable(a):
    return await a

async def _immediate(v):
    return v


# ── wait_for ──────────────────────────────────────────────────────────────────
async def wait_for(fut: Awaitable, timeout: float) -> Any:
    """Wait for *fut* with a *timeout* in seconds."""
    return await asyncio.wait_for(fut, timeout)


# ── ensure_future ─────────────────────────────────────────────────────────────
def ensure_future(coro_or_future) -> Future:
    if isinstance(coro_or_future, Future):
        return coro_or_future
    return create_task(coro_or_future)


# ── run (async entry point) ───────────────────────────────────────────────────
def run(main: Coroutine) -> Any:
    """Run an async main coroutine inside a fresh scheduler."""
    init_scheduler()
    try:
        return _run_coroutine(main)
    finally:
        shutdown_scheduler(wait=True)


# ── AsyncRange / AsyncIterator / AsyncContextManager ─────────────────────────
class AsyncIterator:
    """Base async iterator."""
    def __aiter__(self):
        return self

    async def __anext__(self):
        raise StopAsyncIteration


class AsyncRange(AsyncIterator):
    """Async equivalent of range()."""

    def __init__(self, start: int, stop: Optional[int] = None, step: int = 1):
        if stop is None:
            self._start, self._stop = 0, start
        else:
            self._start, self._stop = start, stop
        self._step    = step
        self._current = self._start

    async def __anext__(self):
        if self._step > 0 and self._current >= self._stop:
            raise StopAsyncIteration
        if self._step < 0 and self._current <= self._stop:
            raise StopAsyncIteration
        value         = self._current
        self._current += self._step
        await sleep(0)
        return value


def async_range(start: int, stop: Optional[int] = None,
                step: int = 1) -> AsyncRange:
    return AsyncRange(start, stop, step)


class AsyncContextManager:
    """Base async context manager."""

    async def __aenter__(self):
        raise NotImplementedError

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        raise NotImplementedError


# ── Native socket factories ───────────────────────────────────────────────────
def create_tcp_socket():
    if _HAS_NATIVE:
        from ._gsyncio_core import GSocket
        return GSocket.tcp()
    raise RuntimeError("Native I/O not available (C extension not built)")


def create_udp_socket():
    if _HAS_NATIVE:
        from ._gsyncio_core import GSocket
        return GSocket.udp()
    raise RuntimeError("Native I/O not available (C extension not built)")


def has_native_io() -> bool:
    return _HAS_NATIVE


__all__ = [
    "create_task", "_run_coroutine", "sleep", "gather", "wait_for",
    "ensure_future", "run", "async_range",
    "AsyncRange", "AsyncIterator", "AsyncContextManager",
    "create_tcp_socket", "create_udp_socket", "has_native_io",
]
