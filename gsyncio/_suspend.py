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


class CancelledError(BaseException):
    """Raised inside a coroutine whose task was cancelled.

    Derives from BaseException, not Exception, for the same reason
    asyncio's does: a bare `except Exception` in user code must not
    accidentally swallow a cancellation and keep the task alive.
    """


class _Sleep:
    """Awaitable that asks the driver to resume us after `delay_ns`."""

    __slots__ = ("delay_ns",)

    def __init__(self, delay_ns: int):
        self.delay_ns = delay_ns

    def __await__(self):
        yield self


class _Yield:
    """Awaitable that gives other ready work a turn, then resumes.

    The cooperative yield point the async model was missing. Without it
    `await sleep(0)` returned straight away, so two coroutines gathered
    together ran strictly one after the other (A0 A1 A2 B0 B1 B2) instead
    of interleaving - there was no way for a coroutine to voluntarily let
    anything else run, and hence no fairness point at all.
    """

    __slots__ = ()

    def __await__(self):
        yield self


class _Waiter:
    """One pending resume, claimed exactly once.

    A suspended coroutine can be woken by two different things racing:
    its timer coming due, and cancel() trying to wake it early. Both must
    not call step() on the same coroutine - a coroutine cannot be resumed
    twice concurrently. `claim()` makes exactly one of them win.
    """

    __slots__ = ("coro", "future", "fired")

    def __init__(self, coro, future):
        self.coro = coro
        self.future = future
        self.fired = False

    def claim(self) -> bool:
        with _CLAIM_LOCK:
            if self.fired:
                return False
            self.fired = True
            return True


# Guards _Waiter.fired only. Taken on resume and on cancel - never on the
# path of a coroutine that runs straight through without suspending.
_CLAIM_LOCK = threading.Lock()


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

    def add(self, delay_ns: int, waiter) -> None:
        self._ensure_thread()
        deadline = time.monotonic_ns() + max(0, delay_ns)
        with self._cv:
            self._seq += 1
            heapq.heappush(self._heap, (deadline, self._seq, waiter))
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
                    _, _, waiter = heapq.heappop(self._heap)
                    # Skip anything cancel() already woke: it resumed the
                    # coroutine and released the token itself, so touching
                    # either here would step the coroutine twice and
                    # double-release. The `finally` below releases exactly
                    # one token per entry we DID claim.
                    if waiter.claim():
                        due.append((step, (waiter.coro, waiter.future)))

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


def _register_waiter(future, waiter) -> None:
    """Record where a coroutine is parked, so cancel() can reach it.

    Best-effort: anything without the attribute (a ResultBox from
    gs.run(), say) simply isn't cancellable, which is fine - nobody holds
    a handle to it.
    """
    try:
        future.waiter = waiter
    except AttributeError:
        pass


def _set_cancelled(future) -> None:
    """Complete a future as cancelled rather than as a plain exception."""
    marker = getattr(future, "mark_cancelled", None)
    if marker is not None:
        marker()
    else:
        future.set_exception(CancelledError())


def resume_cancelled(waiter) -> None:
    """Wake a parked coroutine early to deliver a cancellation.

    Called from Future.cancel(). Only proceeds if it wins the claim
    against the pending timer, so the coroutine is stepped exactly once.
    """
    if not waiter.claim():
        return False
    from .core import task as _task
    try:
        _task(step, waiter.coro, waiter.future, None, CancelledError())
    finally:
        # Balances the token taken when the coroutine parked; the timer
        # entry that still holds this waiter will now skip it.
        _pending_token_release()
    return True


def step(coro, future, send_value=None, throw_exc=None) -> None:
    """Advance `coro` one step and park it again if it suspends.

    Runs on a gsyncio fiber. Returns as soon as the coroutine either
    finishes or asks to wait - it never blocks the worker waiting for
    something to resolve.
    """
    while True:
        # A cancel() that landed while this coroutine was parked is
        # delivered here, at its next resume, as a throw into the
        # coroutine - so `finally` blocks and `except CancelledError`
        # handlers inside it run normally.
        if throw_exc is None and getattr(future, "cancel_requested", False):
            throw_exc = CancelledError()

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
        except CancelledError:
            _set_cancelled(future)
            return
        except BaseException as e:
            future.set_exception(e)
            return
        finally:
            _local.active -= 1

        # ── cooperative yield ────────────────────────────────────────────
        if type(req) is _Yield:
            # Re-queue behind whatever else is already ready, so other
            # coroutines actually get a turn. No token needed: this
            # fiber is still counted until it returns, and the resume is
            # queued before that happens.
            from .core import task as _task
            _task(step, coro, future)
            return

        # ── sleep ────────────────────────────────────────────────────────
        if type(req) is _Sleep:
            # Token first: once this fiber returns, nothing else keeps
            # the task count above zero until the timer fires.
            _pending_token_acquire()
            waiter = _Waiter(coro, future)
            _register_waiter(future, waiter)
            _timers.add(req.delay_ns, waiter)
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
            waiter = _Waiter(coro, future)
            _register_waiter(future, waiter)

            def _on_done(_f, _w=waiter):
                from .core import task as _task
                # cancel() may already have woken this coroutine; only one
                # of us may step it.
                if not _w.claim():
                    return
                try:
                    try:
                        val = _f.result()
                    except BaseException as e:
                        _task(step, _w.coro, _w.future, None, e)
                    else:
                        _task(step, _w.coro, _w.future, val)
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
