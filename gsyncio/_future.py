"""
gsyncio._future — Python-level Future class.

Wraps the C Future (or fallback Future) and makes it awaitable in both
asyncio and gsyncio fiber contexts.
"""
from typing import Any, Callable, List, Optional

from .core import Future as _CFuture, _HAS_CYTHON, current_fiber_id, yield_execution


class Future:
    """
    Awaitable value representing the eventual result of an async operation.

    Compatible with both asyncio and gsyncio fiber contexts.
    """

    # One Future exists per coroutine on the gather()/create_task() paths,
    # so at a million coroutines these three fields are a million dicts
    # and a million empty lists. __slots__ drops the per-instance dict and
    # the callback list is created only if something actually registers
    # one - which nothing does unless the Future is awaited before it
    # resolves.
    __slots__ = ("_inner", "_callbacks", "_done_flag")

    def __init__(self):
        self._inner     = _CFuture()
        self._callbacks: Optional[List[Callable]] = None
        self._done_flag = False

    # ── Status ────────────────────────────────────────────────────────────────

    @property
    def done(self) -> bool:
        # _done_flag first: it is a plain Python bool, whereas _inner.done
        # crosses into C and takes the future's pthread mutex. This is on
        # the hot path of every await.
        return self._done_flag or self._inner.done

    @property
    def cancelled(self) -> bool:
        return self._inner.cancelled

    # ── Result access ─────────────────────────────────────────────────────────

    def result(self, timeout: Optional[float] = None) -> Any:
        return self._inner.result(timeout)

    def exception(self, timeout: Optional[float] = None) -> Optional[Exception]:
        return self._inner.exception(timeout)

    # ── Completion ────────────────────────────────────────────────────────────

    def set_result(self, result: Any) -> None:
        self._inner.set_result(result)
        self._done_flag = True
        self._fire_callbacks()

    def set_exception(self, exc: Exception) -> None:
        self._inner.set_exception(exc)
        self._done_flag = True
        self._fire_callbacks()

    def _fire_callbacks(self):
        cbs = self._callbacks
        if not cbs:
            return
        self._callbacks = None
        for cb in cbs:
            try:
                cb(self)
            except Exception:
                pass

    # ── Callbacks ─────────────────────────────────────────────────────────────

    def add_callback(self, cb: Callable[["Future"], None]) -> None:
        if self.done:
            try:
                cb(self)
            except Exception:
                pass
        elif self._callbacks is None:
            self._callbacks = [cb]
        else:
            self._callbacks.append(cb)

    def remove_callback(self, cb: Callable[["Future"], None]) -> None:
        if self._callbacks is None:
            return
        try:
            self._callbacks.remove(cb)
        except ValueError:
            pass

    # ── Await protocol ────────────────────────────────────────────────────────

    def __await__(self):
        if not self.done:
            from ._suspend import driver_active
            if driver_active():
                # A gsyncio coroutine driver is stepping us, so hand the
                # Future up to it and suspend. It re-steps this coroutine
                # via a done-callback, leaving the worker thread free
                # meanwhile - the alternative below would block that
                # thread until the Future resolved, which is what capped
                # concurrency at the worker count.
                yield self
                return self._inner.result()

            if _HAS_CYTHON and current_fiber_id() != 0:
                # On a native gsyncio fiber: self._inner.result() below
                # already blocks via a true C-level fiber-park
                # (future_wait()) when not done - the fiber-native path
                # never needs asyncio at all.
                pass
            else:
                # Not on a fiber. Don't just block this thread outright -
                # if there's an ambient asyncio loop running (e.g. this
                # Future is being awaited from code under pytest-asyncio,
                # or any app that embeds gsyncio inside an asyncio loop),
                # the thing that's supposed to complete this Future might
                # be a sibling coroutine on that SAME loop/thread.
                # Blocking the thread would starve it and deadlock -
                # cooperating with the loop instead lets that sibling run.
                import asyncio
                try:
                    loop = asyncio.get_running_loop()
                except RuntimeError:
                    loop = None

                if loop is not None:
                    af = loop.create_future()

                    def _on_done(fut):
                        if not af.done():
                            try:
                                loop.call_soon_threadsafe(af.set_result, fut.result())
                            except Exception as e:
                                loop.call_soon_threadsafe(af.set_exception, e)

                    self.add_callback(_on_done)
                    yield from af
                else:
                    # No fiber, no ambient loop - nothing else needs this
                    # thread free, so a plain blocking wait is safe.
                    import threading
                    ev = threading.Event()
                    self.add_callback(lambda _: ev.set())
                    ev.wait()

        try:
            return self._inner.result()
        except Exception:
            raise

    def __iter__(self):
        return self.__await__()

    # ── Repr ──────────────────────────────────────────────────────────────────

    def __repr__(self) -> str:
        if self.done:
            try:
                return f"<Future done result={self.result()!r}>"
            except Exception as e:
                return f"<Future done exception={e!r}>"
        return "<Future pending>"


# ── Helpers ───────────────────────────────────────────────────────────────────

def ensure_future(coro_or_future) -> Future:
    """Wrap *coro_or_future* in a Future if it isn't one already."""
    if isinstance(coro_or_future, Future):
        return coro_or_future
    from ._async import create_task
    return create_task(coro_or_future)


def is_future(obj: Any) -> bool:
    """Return True if *obj* is a gsyncio Future."""
    return isinstance(obj, Future)


__all__ = ["Future", "ensure_future", "is_future"]
