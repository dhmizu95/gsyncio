"""
gsyncio._waitgroup — WaitGroup synchronization primitive.

Wraps the C WaitGroup (or fallback) with a richer Python interface.
"""
import threading

from .core import WaitGroup as _CWaitGroup


class WaitGroup:
    """
    WaitGroup — wait for a collection of concurrent operations to finish.

    Mirrors Go's ``sync.WaitGroup``.

    Example::

        wg = create_wg()
        add(wg, 5)

        for i in range(5):
            def worker(i=i):
                try:
                    do_work(i)
                finally:
                    done(wg)
            gs.task(worker)

        wait(wg)          # blocks until all five workers call done()
    """

    def __init__(self):
        self._wg = _CWaitGroup()

    # ── Core operations ───────────────────────────────────────────────────────

    def add(self, delta: int = 1) -> None:
        """Add *delta* to the counter. Must be called before spawning work."""
        self._wg.add(delta)

    def done(self) -> None:
        """Decrement counter by 1. Call in the finally block of each worker."""
        self._wg.done()

    # ── Waiting ───────────────────────────────────────────────────────────────

    async def wait_async(self) -> None:
        """Async wait — suspends coroutine until counter reaches zero."""
        await self._wg.wait()

    def wait(self) -> None:
        """
        Synchronous wait — blocks the calling thread until counter reaches zero.

        Uses a threading.Condition for efficiency (no busy-loop).
        """
        if hasattr(self._wg, '_condition'):
            # Pure-Python WaitGroup
            with self._wg._condition:
                while self._wg._counter > 0:
                    self._wg._condition.wait(timeout=0.1)
        else:
            # C WaitGroup — poll with sleep (waitgroup_wait blocks the GIL)
            import time
            while self.counter > 0:
                time.sleep(0.001)

    # ── Properties ────────────────────────────────────────────────────────────

    @property
    def counter(self) -> int:
        """Current counter value."""
        return self._wg.counter

    def __repr__(self) -> str:
        return f"<WaitGroup counter={self.counter}>"


# ── Factory & functional API ──────────────────────────────────────────────────

def create_wg() -> WaitGroup:
    """Create a new WaitGroup."""
    return WaitGroup()


def add(wg: WaitGroup, delta: int = 1) -> None:
    """Add *delta* to *wg*'s counter."""
    wg.add(delta)


def done(wg: WaitGroup) -> None:
    """Decrement *wg*'s counter by 1."""
    wg.done()


def wait(wg: WaitGroup) -> None:
    """Block until *wg*'s counter reaches zero."""
    wg.wait()


__all__ = ["WaitGroup", "create_wg", "add", "done", "wait"]
