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

    def __init__(self):
        self._inner     = _CFuture()
        self._callbacks: List[Callable] = []
        self._done_flag = False

    # ── Status ────────────────────────────────────────────────────────────────

    @property
    def done(self) -> bool:
        return self._inner.done or self._done_flag

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
        for cb in self._callbacks:
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
        else:
            self._callbacks.append(cb)

    def remove_callback(self, cb: Callable[["Future"], None]) -> None:
        try:
            self._callbacks.remove(cb)
        except ValueError:
            pass

    # ── Await protocol ────────────────────────────────────────────────────────

    def __await__(self):
        if not self.done:
            import asyncio
            try:
                # Async context (asyncio event loop is running)
                loop = asyncio.get_running_loop()
                af   = loop.create_future()

                def _on_done(fut):
                    if not af.done():
                        try:
                            loop.call_soon_threadsafe(af.set_result,
                                                      fut.result())
                        except Exception as e:
                            loop.call_soon_threadsafe(af.set_exception, e)

                self.add_callback(_on_done)
                yield from af

            except RuntimeError:
                # No running loop — gsyncio fiber context or bare thread
                if _HAS_CYTHON and current_fiber_id() != 0:
                    while not self.done:
                        yield_execution()
                else:
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
