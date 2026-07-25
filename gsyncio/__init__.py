"""
gsyncio — High-Performance Fiber-Based Concurrency for Python
=============================================================

Provides Go-style fiber/goroutine concurrency with:
  • M:N fiber scheduler (C11 + pthreads + io_uring)
  • task/sync model for fire-and-forget parallelism
  • async/await model (asyncio-compatible)
  • Channels (buffered & unbuffered)
  • WaitGroups
  • Select statements
  • Native sockets (GSocket)
  • Full pure-Python fallback when C extension is not built

Usage
-----
    import gsyncio as gs

    # --- Task/Sync model ---
    def worker(n):
        return sum(range(n))

    gs.task(worker, 10_000)
    gs.sync()

    # --- Async/Await model ---
    async def fetch(url):
        await gs.sleep(100)          # 100 ms
        return f"data from {url}"

    async def main():
        tasks = [gs.create_task(fetch(f"url/{i}")) for i in range(10)]
        results = await gs.gather(*tasks)

    gs.run(main())
"""

__version__ = "0.2.0"
__author__  = "gsyncio team"

# ── 1. C extension (Cython) ──────────────────────────────────────────────────
try:
    from ._gsyncio_core import (
        # Classes
        Future,
        Channel,
        WaitGroup,
        TaskRegistry,
        TaskBatch,
        SelectState,
        GSocket,
        # Scheduler
        init_scheduler,
        shutdown_scheduler,
        get_scheduler_stats,
        workers_running,
        get_queued_fiber_count,
        print_scheduler_debug,
        # Fiber primitives
        spawn,
        spawn_direct,
        spawn_batch,
        sleep_ns,
        sleep_us,
        sleep_ms,
        current_fiber_id,
        yield_execution,
        num_workers,
        # Task / sync model
        task,
        task_batch,
        sync,
        sync_timeout,
        task_count,
        task_completed_count,
        run,
        # Atomic helpers
        atomic_task_count,
        atomic_inc_task_count,
        atomic_dec_task_count,
        atomic_all_tasks_complete,
        # Worker management
        check_worker_scaling,
        set_auto_scaling,
        set_energy_efficient_mode,
        get_worker_utilization,
        get_recommended_workers,
        # Async gather / wait
        gather_native,
        wait_for_native,
        # Net
        net_init_,
        net_shutdown_,
        # internal registry ref
        _task_registry,
    )
    _HAS_CYTHON = True
    _HAS_NATIVE_IO = True

except ImportError:
    _HAS_CYTHON = False
    _HAS_NATIVE_IO = False
    from ._fallback import (          # all symbols pulled from fallback module
        Future, Channel, WaitGroup,
        init_scheduler, shutdown_scheduler, get_scheduler_stats,
        spawn, spawn_direct, spawn_batch,
        sleep_ns, sleep_us, sleep_ms,
        current_fiber_id, yield_execution, num_workers,
        task, sync, sync_timeout, task_count, task_completed_count, run,
        atomic_task_count, atomic_inc_task_count,
        atomic_dec_task_count, atomic_all_tasks_complete,
        check_worker_scaling, set_auto_scaling,
        set_energy_efficient_mode, get_worker_utilization, get_recommended_workers,
        workers_running, get_queued_fiber_count, print_scheduler_debug,
    )
    # Stub classes/functions not available in fallback
    class TaskRegistry:
        pass
    class TaskBatch:
        pass
    class SelectState:
        pass
    class GSocket:
        pass
    def gather_native(*args, **kw):
        raise RuntimeError("gather_native requires C extension")
    def wait_for_native(*args, **kw):
        raise RuntimeError("wait_for_native requires C extension")
    def net_init_():
        pass
    def net_shutdown_():
        pass
    _task_registry = None

# ── 2. async/await helpers ────────────────────────────────────────────────────
from ._async import (
    create_task,
    sleep,
    gather,
    wait_for,
    ensure_future,
    async_range,
    AsyncRange,
    AsyncIterator,
    AsyncContextManager,
)

from ._channel import (
    Chan,
    chan,
    create_chan,
    send,
    recv,
    close,
)

# ── 4. WaitGroup helpers ──────────────────────────────────────────────────────
from ._waitgroup import (
    WaitGroup as WaitGroup,   # richer Python class (wraps C WaitGroup)
    create_wg,
    add,
    done,
    wait,
)

# ── 5. Select helpers ─────────────────────────────────────────────────────────
from ._select import (
    SelectCase,
    SelectResult,
    select_recv,
    select_send,
    default,
    select as select,
)

# ── 6. Future helpers ─────────────────────────────────────────────────────────
from ._future import (
    Future as Future,
    ensure_future as ensure_future,
    is_future,
)

# ── 7. Native I/O ─────────────────────────────────────────────────────────────
from .native_io import (
    NativeSocket,
    NativeEventLoop,
    _HAS_NATIVE_IO as _HAS_NATIVE_IO,
)

# ── 8. Public API ─────────────────────────────────────────────────────────────
__all__ = [
    "__version__", "__author__",

    # Flags
    "_HAS_CYTHON", "_HAS_NATIVE_IO",

    # Core C classes
    "Channel", "WaitGroup", "TaskRegistry", "TaskBatch", "SelectState", "GSocket",

    # Scheduler
    "init_scheduler", "shutdown_scheduler", "get_scheduler_stats",
    "workers_running", "get_queued_fiber_count", "print_scheduler_debug",

    # Fiber primitives
    "spawn", "spawn_direct", "spawn_batch",
    "sleep_ns", "sleep_us", "sleep_ms",
    "current_fiber_id", "yield_execution", "num_workers",

    # Task / sync model
    "task", "task_batch", "sync", "sync_timeout",
    "task_count", "task_completed_count", "run",

    # Atomics
    "atomic_task_count", "atomic_inc_task_count",
    "atomic_dec_task_count", "atomic_all_tasks_complete",

    # Worker management
    "check_worker_scaling", "set_auto_scaling",
    "set_energy_efficient_mode", "get_worker_utilization", "get_recommended_workers",

    # Async/await
    "create_task", "sleep", "gather", "wait_for",
    "ensure_future", "async_range", "AsyncRange", "AsyncIterator", "AsyncContextManager",
    "gather_native", "wait_for_native",

    # Channel API
    "Chan", "chan", "create_chan", "send", "recv", "close",

    # WaitGroup API
    "create_wg", "add", "done", "wait",

    # Select API
    "SelectCase", "SelectResult", "select_recv", "select_send", "default", "select",

    # Future API
    "Future", "is_future",

    # Native I/O
    "NativeSocket", "NativeEventLoop",

    # Net
    "net_init_", "net_shutdown_",
]
