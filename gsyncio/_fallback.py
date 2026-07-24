"""
gsyncio._fallback — Pure-Python fallback implementations.

Used automatically when the Cython extension (_gsyncio_core.so) is not built.
All symbols match what the C extension exports so the Python layer is transparent.
"""
import os
import threading
import time
from typing import Any, List, Optional

# ── Threading event shared by sync() / sync_timeout() ────────────────────────
_all_done_event = threading.Event()
_all_done_event.set()   # Initially "done" (nothing pending)

# ── Lock-free-ish atomic counter ──────────────────────────────────────────────
_atomic_counter = 0
_atomic_lock    = threading.Lock()


def _atomic_get():
    with _atomic_lock:
        return _atomic_counter


def _atomic_inc():
    global _atomic_counter
    with _atomic_lock:
        _atomic_counter += 1
        _all_done_event.clear()
        return _atomic_counter


def _atomic_dec():
    global _atomic_counter
    with _atomic_lock:
        _atomic_counter = max(0, _atomic_counter - 1)
        if _atomic_counter == 0:
            _all_done_event.set()
        return _atomic_counter


# ── Public atomic API (mirrors C extension) ────────────────────────────────────
def atomic_task_count() -> int:
    return _atomic_get()

def atomic_inc_task_count() -> int:
    return _atomic_inc()

def atomic_dec_task_count() -> int:
    return _atomic_dec()

def atomic_all_tasks_complete() -> bool:
    return _atomic_get() == 0


# ── Future ────────────────────────────────────────────────────────────────────
class Future:
    """Pure-Python future — thread-safe, awaitable."""

    def __init__(self):
        self._result    = None
        self._exception = None
        self._done      = False
        self._callbacks: List = []
        self._lock      = threading.Lock()
        self._event     = threading.Event()

    @property
    def done(self) -> bool:
        return self._done

    @property
    def cancelled(self) -> bool:
        return False

    def result(self, timeout: Optional[float] = None):
        if not self._done:
            self._event.wait(timeout)
        if not self._done:
            raise RuntimeError("Future not done")
        if self._exception:
            raise self._exception
        return self._result

    def exception(self, timeout: Optional[float] = None):
        if not self._done:
            self._event.wait(timeout)
        if not self._done:
            raise RuntimeError("Future not done")
        return self._exception

    def set_result(self, result):
        with self._lock:
            if self._done:
                raise RuntimeError("Future already completed")
            self._result = result
            self._done   = True
            self._event.set()
            for cb in self._callbacks:
                _safe_call(cb, self)

    def set_exception(self, exc: Exception):
        with self._lock:
            if self._done:
                raise RuntimeError("Future already completed")
            self._exception = exc
            self._done      = True
            self._event.set()
            for cb in self._callbacks:
                _safe_call(cb, self)

    def add_callback(self, cb):
        with self._lock:
            if self._done:
                _safe_call(cb, self)
            else:
                self._callbacks.append(cb)

    def __await__(self):
        import asyncio
        if not self._done:
            try:
                loop = asyncio.get_running_loop()
                af = loop.create_future()

                def _on_done(fut):
                    if not af.done():
                        try:
                            loop.call_soon_threadsafe(af.set_result, fut.result())
                        except Exception as e:
                            loop.call_soon_threadsafe(af.set_exception, e)

                self.add_callback(_on_done)
                yield from af
            except RuntimeError:
                # No running loop — busy-wait
                while not self._done:
                    pass
        return self._result

    def __iter__(self):
        return self.__await__()

    def __repr__(self):
        if self._done:
            try:
                return f"<Future done result={self.result()!r}>"
            except Exception as e:
                return f"<Future done exception={e!r}>"
        return "<Future pending>"


def _safe_call(fn, *args):
    try:
        fn(*args)
    except Exception:
        pass


# ── Channel ───────────────────────────────────────────────────────────────────
class Channel:
    """Pure-Python buffered channel."""

    def __init__(self, capacity: int = 0):
        self._capacity = capacity
        self._queue: List = []
        self._closed   = False
        self._lock     = threading.Lock()
        self._not_empty = threading.Condition(self._lock)
        self._not_full  = threading.Condition(self._lock)

    @property
    def capacity(self) -> int:
        return self._capacity

    @property
    def size(self) -> int:
        with self._lock:
            return len(self._queue)

    @property
    def closed(self) -> bool:
        return self._closed

    async def send(self, value):
        import asyncio
        while True:
            with self._lock:
                if self._closed:
                    raise RuntimeError("send on closed channel")
                if self._capacity == 0 or len(self._queue) < self._capacity:
                    self._queue.append(value)
                    self._not_empty.notify_all()
                    return
            await asyncio.sleep(0)

    async def recv(self):
        import asyncio
        while True:
            with self._lock:
                if self._queue:
                    v = self._queue.pop(0)
                    self._not_full.notify_all()
                    return v
                if self._closed:
                    raise StopAsyncIteration("Channel closed")
            await asyncio.sleep(0)

    def send_nowait(self, value) -> bool:
        with self._lock:
            if self._closed:
                return False
            if self._capacity > 0 and len(self._queue) >= self._capacity:
                return False
            self._queue.append(value)
            self._not_empty.notify_all()
            return True

    def recv_nowait(self):
        with self._lock:
            if not self._queue:
                return None
            v = self._queue.pop(0)
            self._not_full.notify_all()
            return v

    def close(self):
        with self._lock:
            self._closed = True
            self._not_empty.notify_all()
            self._not_full.notify_all()

    def __len__(self):
        return self.size

    def __bool__(self):
        return not self._closed


# ── WaitGroup ─────────────────────────────────────────────────────────────────
class WaitGroup:
    """Pure-Python WaitGroup."""

    def __init__(self):
        self._counter   = 0
        self._condition = threading.Condition()

    def add(self, delta: int = 1):
        with self._condition:
            self._counter += delta
            if self._counter < 0:
                raise RuntimeError("WaitGroup counter < 0")

    def done(self):
        with self._condition:
            self._counter -= 1
            if self._counter <= 0:
                self._counter = 0
                self._condition.notify_all()

    async def wait(self):
        import asyncio
        while self.counter > 0:
            await asyncio.sleep(0.001)

    @property
    def counter(self) -> int:
        with self._condition:
            return self._counter


# ── Scheduler stubs ───────────────────────────────────────────────────────────
def init_scheduler(num_workers: int = 0, max_fibers: int = 1_000_000,
                   work_stealing: int = 1, stack_mode: int = 1, **kw):
    pass   # no-op: threading handles concurrency


def shutdown_scheduler(wait: int = 1):
    if wait:
        _all_done_event.wait()


def get_scheduler_stats() -> dict:
    return {
        'total_fibers_created': 0, 'total_fibers_completed': 0,
        'total_context_switches': 0, 'total_work_steals': 0,
        'current_active_fibers': _atomic_get(),
        'current_ready_fibers': 0,
    }

def workers_running() -> bool:
    return not _all_done_event.is_set()

def get_queued_fiber_count() -> int:
    return _atomic_get()

def print_scheduler_debug():
    print(f"[gsyncio fallback] active tasks: {_atomic_get()}")


# ── Spawn helpers ─────────────────────────────────────────────────────────────
def _spawn_thread(func, args):
    """Start a daemon thread that counts itself in/out."""
    _atomic_inc()

    def _run():
        try:
            func(*args)
        finally:
            _atomic_dec()

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t


def spawn(func, *args):
    return _spawn_thread(func, args)

def spawn_direct(func, args):
    _spawn_thread(func, args)

def spawn_batch(funcs_and_args: List) -> List:
    return [_spawn_thread(f, a) for f, a in funcs_and_args]

def spawn_batch_fast(funcs_and_args: List):
    for f, a in funcs_and_args:
        _spawn_thread(f, a)

def spawn_batch_ultra_fast(funcs_and_args: List, store_fiber_ids: int = 0) -> int:
    spawn_batch_fast(funcs_and_args)
    return len(funcs_and_args)


# ── Sleep ─────────────────────────────────────────────────────────────────────
def sleep_ms(ms: float):
    time.sleep(ms / 1000.0)

def sleep_us(us: float):
    time.sleep(us / 1_000_000.0)

def sleep_ns(ns: float):
    time.sleep(ns / 1_000_000_000.0)


# ── Fiber identity ────────────────────────────────────────────────────────────
def current_fiber_id() -> int:
    return threading.current_thread().ident or 0

def yield_execution():
    pass  # no-op

def num_workers() -> int:
    return os.cpu_count() or 1


# ── Task model ────────────────────────────────────────────────────────────────
def task(func, *args, **kwargs):
    """Spawn a fire-and-forget task."""
    import inspect
    if inspect.iscoroutine(func):
        _spawn_thread(_run_coro_sync, (func,))
    else:
        _spawn_thread(func, args)
    return True

def _run_coro_sync(coro):
    import asyncio
    asyncio.run(coro)

def task_batch(funcs_and_args: List) -> int:
    spawn_batch(funcs_and_args)
    return len(funcs_and_args)

def sync():
    """Block until all tasks spawned via task()/spawn() complete."""
    _all_done_event.wait()

def sync_timeout(timeout: float) -> bool:
    return _all_done_event.wait(timeout=timeout)

def task_count() -> int:
    return _atomic_get()

def task_completed_count() -> int:
    return 0

def run(func, *args, **kwargs):
    """Run a function or coroutine in the gsyncio runtime."""
    import inspect
    if inspect.iscoroutine(func):
        import asyncio
        return asyncio.run(func)
    result = func(*args, **kwargs)
    if inspect.iscoroutine(result):
        import asyncio
        return asyncio.run(result)
    return result


# ── Worker management stubs ───────────────────────────────────────────────────
def check_worker_scaling(): pass
def set_auto_scaling(enabled: bool): pass
def set_energy_efficient_mode(enabled: bool): pass
def get_worker_utilization() -> float: return 0.0
def get_recommended_workers() -> int: return os.cpu_count() or 1
