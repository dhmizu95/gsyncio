"""
gsyncio.core — Bridge module.

Tries to load the Cython extension; falls back to _fallback.
All other gsyncio modules import from here so they don't need to
repeat the try/except dance.
"""

try:
    from ._gsyncio_core import (
        Future,
        Channel,
        WaitGroup,
        TaskRegistry,
        TaskBatch,
        init_scheduler,
        shutdown_scheduler,
        get_scheduler_stats,
        spawn,
        spawn_direct,
        spawn_batch,
        spawn_coro_batch,
        sleep_ns,
        sleep_us,
        sleep_ms,
        current_fiber_id,
        yield_execution,
        num_workers,
        task,
        task_batch,
        sync,
        sync_timeout,
        task_count,
        task_completed_count,
        run,
        atomic_task_count,
        atomic_inc_task_count,
        atomic_dec_task_count,
        atomic_all_tasks_complete,
        check_worker_scaling,
        set_auto_scaling,
        set_energy_efficient_mode,
        get_worker_utilization,
        get_recommended_workers,
    )
    from . import _gsyncio_core as _core_mod

    def _get_task_registry():
        return _core_mod._task_registry

    _HAS_CYTHON = True

except ImportError:
    from ._fallback import (
        Future,
        Channel,
        WaitGroup,
        init_scheduler,
        shutdown_scheduler,
        get_scheduler_stats,
        spawn,
        spawn_direct,
        spawn_batch,
        sleep_ns,
        sleep_us,
        sleep_ms,
        current_fiber_id,
        yield_execution,
        num_workers,
        task,
        task_batch,
        sync,
        sync_timeout,
        task_count,
        task_completed_count,
        run,
        atomic_task_count,
        atomic_inc_task_count,
        atomic_dec_task_count,
        atomic_all_tasks_complete,
        check_worker_scaling,
        set_auto_scaling,
        set_energy_efficient_mode,
        get_worker_utilization,
        get_recommended_workers,
    )

    def _get_task_registry():
        return None

    _HAS_CYTHON = False


__all__ = [
    "Future", "Channel", "WaitGroup",
    "init_scheduler", "shutdown_scheduler", "get_scheduler_stats",
    "spawn", "spawn_direct", "spawn_batch",
    "sleep_ns", "sleep_us", "sleep_ms",
    "current_fiber_id", "yield_execution", "num_workers",
    "task", "task_batch", "sync", "sync_timeout",
    "task_count", "task_completed_count", "run",
    "atomic_task_count", "atomic_inc_task_count",
    "atomic_dec_task_count", "atomic_all_tasks_complete",
    "check_worker_scaling", "set_auto_scaling",
    "set_energy_efficient_mode", "get_worker_utilization", "get_recommended_workers",
    "_HAS_CYTHON", "_get_task_registry",
]
