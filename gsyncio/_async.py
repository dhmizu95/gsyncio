"""
gsyncio._async — Async/await helpers.

Works with both the C extension and pure-Python fallback. Coroutines are
driven by gsyncio's own fiber scheduler, not asyncio: gsyncio-native
awaitables (Future, sleep, channel, WaitGroup) resolve via a true C-level
block when running on a fiber, so a coroutine running inside one never
actually needs to yield control back to Python - `coro.send(None)`
blocks transitively all the way to completion. Only bare coroutines get
wrapped into new fibers here; anything already awaitable (a gsyncio
Future, a raw asyncio Task passed in by caller code that's itself
running under a real asyncio loop) is awaited as-is, deferring to
whatever is actually driving the surrounding coroutine.
"""
import inspect
import time
from typing import Any, Coroutine, List, Optional, Awaitable

try:
    from ._gsyncio_core import GSocket, sleep_ns as _c_sleep_ns
    from ._gsyncio_core import spawn_coro_batch as _spawn_coro_batch
    _HAS_NATIVE = True
except ImportError:
    _HAS_NATIVE = False
    _spawn_coro_batch = None

from .core import Future, sleep_ms, sleep_ns, init_scheduler, shutdown_scheduler, _HAS_CYTHON, task as _task


def _drive_coro(coro) -> Any:
    """Run *coro* to completion on the calling fiber.

    Works because every gsyncio-native awaitable blocks synchronously in
    C when called from a fiber - so this one `send(None)` transitively
    blocks all the way to the coroutine's final return. If the coroutine
    yields for real (it awaited something that isn't gsyncio-native),
    there's no event loop here to service it - that's a usage error, not
    something to silently hang on.
    """
    try:
        coro.send(None)
    except StopIteration as e:
        return e.value
    else:
        raise RuntimeError(
            "coroutine awaited something that isn't gsyncio-native while "
            "running under gsyncio's native driver - no asyncio event "
            "loop is running to service it"
        )


# ── create_task ───────────────────────────────────────────────────────────────
def create_task(coro: Coroutine) -> Future:
    """Wrap a coroutine in a gsyncio Future and run it on its own fiber."""
    future = Future()

    def _driver():
        try:
            result = _drive_coro(coro)
            future.set_result(result)
        except BaseException as e:
            future.set_exception(e)

    _task(_driver)
    return future


# ── sleep ─────────────────────────────────────────────────────────────────────
async def sleep(ms: float) -> None:
    """Sleep for *ms* milliseconds (fiber-aware when on a C fiber)."""
    from .core import current_fiber_id
    if _HAS_CYTHON and current_fiber_id() != 0:
        # Running on a native gsyncio fiber - real blocking C sleep.
        _c_sleep_ns(int(ms * 1_000_000))
    else:
        # Not on a fiber (e.g. called directly from a plain thread or an
        # ambient asyncio loop, without gs.run()/gs.create_task()) - a
        # plain blocking sleep, no asyncio.
        time.sleep(ms / 1000.0)


# ── gather ────────────────────────────────────────────────────────────────────
async def gather(*awaitables: Awaitable,
                 return_exceptions: bool = False) -> List[Any]:
    """Concurrently await multiple awaitables.

    Bare coroutines are batched: gather() sees the whole set up front
    (unlike create_task(), which has to hand back a Future for one
    coroutine immediately, before any batch exists), so they're driven
    in chunks via spawn_coro_batch() - one fiber and one GIL acquisition
    per chunk instead of one of each per coroutine. This is the same
    chunking idea as spawn_batch_fast() (see its docstring), just for
    coroutines instead of plain calls. Falls back to create_task() one
    at a time if the native batch path isn't available (pure-Python
    fallback build). Anything already awaitable (a gsyncio Future, a raw
    asyncio Task/Future, any object with __await__) is awaited as-is -
    deferring to whatever is actually driving this coroutine (gsyncio's
    native driver, or an ambient asyncio loop if this is itself called
    from asyncio-driven code).
    """
    awaitables = list(awaitables)
    coro_indices = [i for i, a in enumerate(awaitables) if inspect.iscoroutine(a)]

    prepared = list(awaitables)
    if coro_indices:
        if _spawn_coro_batch is not None:
            batched_futures = _spawn_coro_batch([awaitables[i] for i in coro_indices])
            for i, fut in zip(coro_indices, batched_futures):
                prepared[i] = fut
        else:
            for i in coro_indices:
                prepared[i] = create_task(awaitables[i])

    results = []
    for a in prepared:
        if return_exceptions:
            try:
                results.append(await a)
            except Exception as e:
                results.append(e)
        else:
            results.append(await a)
    return results


# ── wait_for ──────────────────────────────────────────────────────────────────
async def wait_for(fut: Awaitable, timeout: float) -> Any:
    """Wait for *fut* with a *timeout* in seconds."""
    if inspect.iscoroutine(fut):
        fut = create_task(fut)
    deadline = time.monotonic() + timeout
    while not fut.done:
        if time.monotonic() >= deadline:
            raise TimeoutError("wait_for() timed out")
        await sleep(1)
    return fut.result()


# ── ensure_future ─────────────────────────────────────────────────────────────
def ensure_future(coro_or_future) -> Future:
    if isinstance(coro_or_future, Future):
        return coro_or_future
    return create_task(coro_or_future)


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
    "create_task", "sleep", "gather", "wait_for",
    "ensure_future", "async_range",
    "AsyncRange", "AsyncIterator", "AsyncContextManager",
    "create_tcp_socket", "create_udp_socket", "has_native_io",
]
