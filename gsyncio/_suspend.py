"""
gsyncio._suspend — real coroutine suspension.

Before this, a coroutine running under gsyncio could not suspend at all.
`_drive_coro()` did a single `coro.send(None)` and every gsyncio-native
await (sleep, Future) blocked the *OS worker thread* until it resolved
(see scheduler_sleep_ns()'s own comment: "A real OS sleep is the only
correct wait here"). That made the concurrency ceiling equal to the
worker-thread count: 1,000 coroutines each sleeping 100 ms took ~1.06 s
instead of ~0.1 s, and the only way to hold more was to grow more OS
threads - one per suspended coroutine.

Here a coroutine that awaits something unresolved *yields* a suspend
request instead. The driver parks the (coroutine, future) pair against
whatever it is waiting for - a deadline on the timer wheel, or a
callback on a Future - and returns, freeing the worker immediately. When
the wait resolves, the coroutine is re-spawned and stepped again. One
background thread services every pending timer, so N sleeping coroutines
cost one thread rather than N.

This needs no fiber stack-switching: the coroutine object *is* the saved
state, which is exactly what makes the Python-coroutine model cheaper
than real fibers for this. Plain `gs.task()` bodies are unaffected -
they are ordinary function calls with no resumable state, so they still
block their worker (see sleep() in _async.py).
"""
import heapq
import threading
import time
from typing import Any

from .core import _HAS_CYTHON

try:
    from ._gsyncio_core import (
        atomic_inc_task_count as _inc_tasks,
        atomic_dec_task_count as _dec_tasks,
    )
except ImportError:  # pure-Python fallback build
    _inc_tasks = _dec_tasks = None


# ── "is a coroutine being driven on this thread right now?" ──────────────────
# Set only while _step() is inside coro.send(). Awaitables consult it to
# decide between yielding a suspend request (there is a driver above us
# that knows how to park us) and their legacy blocking behavior (there
# isn't - e.g. a Future awaited straight from a plain gs.task() fiber).
_local = threading.local()


def driver_active() -> bool:
    return getattr(_local, "active", 0) > 0


class _Sleep:
    """Awaitable that asks the driver to resume us after `delay_ns`."""

    __slots__ = ("delay_ns",)

    def __init__(self, delay_ns: int):
        self.delay_ns = delay_ns

    def __await__(self):
        yield self


# ── timer wheel ──────────────────────────────────────────────────────────────

class _TimerWheel:
    """One thread, one heap, any number of sleeping coroutines.

    Replaces "block a worker thread per sleeping coroutine". The thread
    is started lazily on first use and is a daemon, so importing gsyncio
    costs nothing and a program that never sleeps never spawns it.
    """

    def __init__(self):
        self._heap = []
        self._seq = 0
        self._cv = threading.Condition()
        self._thread = None

    def _ensure_thread(self):
        if self._thread is None:
            with self._cv:
                if self._thread is None:
                    self._thread = threading.Thread(
                        target=self._run, name="gsyncio-timers", daemon=True)
                    self._thread.start()

    def add(self, delay_ns: int, coro, future) -> None:
        self._ensure_thread()
        deadline = time.monotonic_ns() + max(0, delay_ns)
        with self._cv:
            self._seq += 1
            heapq.heappush(self._heap, (deadline, self._seq, coro, future))
            # Only worth waking the thread if this is the new earliest
            # deadline; otherwise it is already sleeping for less time.
            if self._heap[0][1] == self._seq:
                self._cv.notify()

    def _run(self):
        from ._gsyncio_core import spawn as _spawn
        while True:
            due = []
            with self._cv:
                if not self._heap:
                    self._cv.wait(60.0)
                    continue
                now = time.monotonic_ns()
                if self._heap[0][0] > now:
                    self._cv.wait((self._heap[0][0] - now) / 1e9)
                    continue
                # Drain everything already due in one pass so a large
                # batch of same-deadline sleepers costs one wake-up.
                while self._heap and self._heap[0][0] <= now:
                    _, _, coro, future = heapq.heappop(self._heap)
                    due.append((step, (coro, future)))

            if not due:
                continue
            # Resume the whole batch through the chunked spawn path
            # rather than one gs.task() per sleeper. This thread is the
            # only one servicing timers, so a per-sleeper spawn makes it
            # the bottleneck the moment many coroutines share a deadline
            # - which is exactly what a fan-out of sleepers looks like.
            try:
                _spawn(due)
            finally:
                # Tokens are released only after the resumes are queued,
                # so the scheduler's task count never dips to zero
                # between the two and lets sync() return early.
                for _ in due:
                    _pending_token_release()


_timers = _TimerWheel()


# ── the driver ───────────────────────────────────────────────────────────────

def _pending_token_acquire():
    """Keep gs.sync() waiting while a coroutine is suspended.

    sync() waits on the scheduler's task count. A suspended coroutine has
    no live fiber - its fiber returned so the worker could go do
    something else - so without a token standing in for it, the count
    would hit zero and sync() would return while coroutines were still
    pending.
    """
    if _inc_tasks is not None:
        _inc_tasks()


def _pending_token_release():
    if _dec_tasks is not None:
        _dec_tasks()


def step(coro, future, send_value=None, throw_exc=None) -> None:
    """Advance `coro` one step and park it again if it suspends.

    Runs on a gsyncio fiber. Returns as soon as the coroutine either
    finishes or asks to wait - it never blocks the worker waiting for
    something to resolve.
    """
    while True:
        _local.active = getattr(_local, "active", 0) + 1
        try:
            if throw_exc is not None:
                exc, throw_exc = throw_exc, None
                req = coro.throw(exc)
            else:
                req = coro.send(send_value)
            send_value = None
        except StopIteration as e:
            future.set_result(e.value)
            return
        except BaseException as e:
            future.set_exception(e)
            return
        finally:
            _local.active -= 1

        # ── sleep ────────────────────────────────────────────────────────
        if type(req) is _Sleep:
            # Token first: once this fiber returns, nothing else keeps
            # the task count above zero until the timer fires.
            _pending_token_acquire()
            _timers.add(req.delay_ns, coro, future)
            return

        # ── another gsyncio Future ───────────────────────────────────────
        if hasattr(req, "add_callback") and hasattr(req, "done"):
            if req.done:
                # Already resolved between the yield and here - just loop
                # rather than paying a re-spawn.
                try:
                    send_value = req.result()
                except BaseException as e:
                    throw_exc = e
                continue

            _pending_token_acquire()

            def _on_done(_f, _coro=coro, _fut=future):
                from .core import task as _task
                try:
                    try:
                        val = _f.result()
                    except BaseException as e:
                        _task(step, _coro, _fut, None, e)
                    else:
                        _task(step, _coro, _fut, val)
                finally:
                    _pending_token_release()

            req.add_callback(_on_done)
            return

        # ── anything else ────────────────────────────────────────────────
        future.set_exception(RuntimeError(
            f"coroutine awaited {req!r}, which isn't gsyncio-native - "
            "no asyncio event loop is running to service it"
        ))
        return


class ResultBox:
    """Minimal set_result/set_exception sink for step().

    Used by gs.run(), which just needs the outcome of one coroutine and
    has nobody to await it. Deliberately not a Future: the C Future's
    exception() returns its raw NULL exception pointer as a Python object
    when the future completed with a *result*, so asking a
    result-completed Future whether it holds an exception is not safe.
    """

    __slots__ = ("value", "exc", "done")

    def __init__(self):
        self.value = None
        self.exc = None
        self.done = False

    def set_result(self, value: Any) -> None:
        self.value = value
        self.done = True

    def set_exception(self, exc: BaseException) -> None:
        self.exc = exc
        self.done = True

    def unwrap(self) -> Any:
        if self.exc is not None:
            raise self.exc
        return self.value


__all__ = ["step", "driver_active", "_Sleep", "_timers", "ResultBox"]
