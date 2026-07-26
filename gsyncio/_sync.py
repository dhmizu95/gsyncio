"""
gsyncio._sync — async synchronization primitives.

Semaphore, BoundedSemaphore, Lock and Event, built on the same
suspension machinery as sleep() (see gsyncio._suspend): a coroutine that
has to wait parks on a Future and frees its worker thread, rather than
blocking it.

These are for coroutines. A plain gs.task() function body has no
resumable state to park, so use a channel or a WaitGroup there.
"""
import threading
from typing import Deque, Optional
from collections import deque

from ._future import Future


class Semaphore:
    """Counting semaphore for coroutines.

        sem = gs.Semaphore(10)
        async with sem:
            ...

    Fair: waiters are served in arrival order, so a steady stream of
    acquirers cannot starve one that has been waiting.
    """

    def __init__(self, value: int = 1):
        if value < 0:
            raise ValueError("Semaphore initial value must be >= 0")
        self._value = value
        # Guards _value and _waiters against acquire/release running on
        # different worker threads at once.
        self._lock = threading.Lock()
        self._waiters: Deque[Future] = deque()

    def locked(self) -> bool:
        return self._value == 0

    async def acquire(self) -> bool:
        while True:
            with self._lock:
                if self._value > 0:
                    self._value -= 1
                    return True
                waiter = Future()
                self._waiters.append(waiter)

            try:
                await waiter
            except BaseException:
                # Cancelled (or failed) while queued - make sure the
                # permit handed to us, if any, is not lost.
                with self._lock:
                    try:
                        self._waiters.remove(waiter)
                    except ValueError:
                        # Already popped by release(), which means a
                        # permit was handed to us. Give it back.
                        if waiter.done and not waiter.cancelled:
                            self._value += 1
                            self._wake_one_locked()
                raise
            # Woken with a permit already transferred to us by release().
            return True

    def _wake_one_locked(self) -> None:
        """Hand one permit to the longest-waiting live waiter."""
        while self._waiters:
            waiter = self._waiters.popleft()
            if waiter.cancelled or waiter.done:
                continue
            self._value -= 1
            waiter.set_result(True)
            return

    def release(self) -> None:
        with self._lock:
            self._value += 1
            self._wake_one_locked()

    async def __aenter__(self):
        await self.acquire()
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.release()
        return False


class BoundedSemaphore(Semaphore):
    """Semaphore that refuses to be released above its initial value."""

    def __init__(self, value: int = 1):
        super().__init__(value)
        self._initial = value

    def release(self) -> None:
        with self._lock:
            if self._value >= self._initial:
                raise ValueError("BoundedSemaphore released too many times")
            self._value += 1
            self._wake_one_locked()


class Lock(Semaphore):
    """Mutual exclusion for coroutines - a Semaphore(1).

        lock = gs.Lock()
        async with lock:
            ...
    """

    def __init__(self):
        super().__init__(1)


class Event:
    """One-shot flag coroutines can wait on."""

    def __init__(self):
        self._set = False
        self._lock = threading.Lock()
        self._waiters: list = []

    def is_set(self) -> bool:
        return self._set

    def set(self) -> None:
        with self._lock:
            if self._set:
                return
            self._set = True
            waiters, self._waiters = self._waiters, []
        for w in waiters:
            if not w.done:
                w.set_result(True)

    def clear(self) -> None:
        with self._lock:
            self._set = False

    async def wait(self) -> bool:
        with self._lock:
            if self._set:
                return True
            waiter = Future()
            self._waiters.append(waiter)
        await waiter
        return True


__all__ = ["Semaphore", "BoundedSemaphore", "Lock", "Event"]
