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
    from ._gsyncio_core import spawn as _spawn
    _HAS_NATIVE = True
except ImportError:
    _HAS_NATIVE = False
    _spawn = None

from .core import sleep_ms, sleep_ns, init_scheduler, shutdown_scheduler, _HAS_CYTHON, task as _task

# The Python-level Future, NOT core's C Future. The C one's __await__
# resolves by calling self.result() -> future_wait() -> pthread_cond_wait,
# i.e. it blocks the worker thread until the future completes. Awaiting a
# C Future from a driven coroutine therefore skipped the suspension
# machinery entirely and serialised the whole drain on one worker
# (measured: 74 us per await at 200k, versus 0.21 us for an await that
# goes through the driver). The Python Future yields to the driver
# instead - see _future.Future.__await__.
from ._future import Future

if _spawn is None:  # pure-Python fallback build has no chunked spawn
    def _spawn(pairs):
        for fn, args in pairs:
            _task(fn, *args)


from ._suspend import step as _step, driver_active as _driver_active, _Sleep


# ── create_task ───────────────────────────────────────────────────────────────
def create_task(coro: Coroutine) -> Future:
    """Wrap a coroutine in a gsyncio Future and start it on a fiber.

    The coroutine is *stepped*, not run to completion in place: if it
    awaits something unresolved it suspends and frees the worker, and is
    resumed later (see gsyncio._suspend).
    """
    future = Future()
    _task(_step, coro, future)
    return future


# ── sleep ─────────────────────────────────────────────────────────────────────
async def sleep(ms: float) -> None:
    """Sleep for *ms* milliseconds.

    Inside a coroutine this genuinely suspends - the worker thread is
    released and reused, so the number of concurrently sleeping
    coroutines is not bounded by the worker count.

    Called from a plain `gs.task()` function body there is nothing to
    suspend (a function call has no resumable state, unlike a
    coroutine), so it stays a real blocking sleep on that worker. That
    is the Go-style path: use more workers, or use a coroutine.
    """
    if ms <= 0:
        return
    if _driver_active():
        await _Sleep(int(ms * 1_000_000))
        return

    from .core import current_fiber_id
    if _HAS_CYTHON and current_fiber_id() != 0:
        # On a fiber but not inside a driven coroutine - blocking C sleep.
        _c_sleep_ns(int(ms * 1_000_000))
    else:
        # Not on a fiber (plain thread, or an ambient asyncio loop
        # without gs.run()/gs.create_task()) - plain blocking sleep.
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
    chunking idea as spawn() (see its docstring), just for
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
        # Start every coroutine's FIRST step in chunked fibers: spawn()
        # groups them so one GIL acquisition covers a whole chunk, which
        # is where nearly all of the batching win came from. Coroutines
        # that run straight through finish inside that chunk and cost
        # nothing extra; ones that suspend park themselves and get their
        # own fiber on resume. The old path used the C chunk driver,
        # which did a single send() per coroutine and therefore could not
        # let any of them suspend at all.
        futs = [Future() for _ in coro_indices]
        for i, fut in zip(coro_indices, futs):
            prepared[i] = fut
        _spawn([(_step, (awaitables[i], fut))
                for i, fut in zip(coro_indices, futs)])

    results = []
    for a in prepared:
        # Skip the await entirely for anything already resolved. By the
        # time gather() starts collecting, the chunked spawn above has
        # usually finished most of the batch, and `await` on a done
        # Future still costs a generator object plus a yield/resume round
        # trip through the driver for no reason.
        if type(a) is Future and a.done:
            if return_exceptions:
                try:
                    results.append(a.result())
                except Exception as e:
                    results.append(e)
            else:
                results.append(a.result())
            continue

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
