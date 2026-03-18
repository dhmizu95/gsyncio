"""
gsyncio - High-Performance Fiber-Based Concurrency for Python

gsyncio provides Viper-like fiber-based concurrency primitives, designed
to override and replace Python's asyncio with significantly better
performance. It provides both fire-and-forget parallel work (task/sync)
and I/O-bound asynchronous operations (async/await) with minimal overhead.

Features:
- M:N fiber scheduler with work-stealing
- Task/Sync model for fire-and-forget parallelism
- Async/Await model for I/O-bound operations
- Channels for message-passing between fibers
- WaitGroups for synchronization
- Select statements for channel multiplexing
- Full asyncio compatibility (with monkey-patching)

Usage:
    import gsyncio as gs
    
    # Task/Sync model
    def worker(n):
        result = sum(range(n))
        print(f"Result: {result}")
    
    gs.task(worker, 1000)
    gs.sync()
    
    # Async/Await model
    async def fetch(url):
        await gs.sleep(100)
        return f"Data from {url}"
    
    async def main():
        results = await gs.gather(*[fetch(f"url/{i}") for i in range(100)])
        print(f"Fetched {len(results)} items")
    
    gs.run(main())

Installation:
    pip install gsyncio

For more information, see https://github.com/gsyncio/gsyncio
"""

__version__ = '0.1.0'
__author__ = 'gsyncio team'

# Import core components
from .core import (
    Future,
    Channel,
    WaitGroup,
    init_scheduler,
    shutdown_scheduler,
    get_scheduler_stats,
    spawn,
    spawn_batch,
    spawn_batch_fast,
    sleep_ns,
    sleep_us,
    sleep_ms,
    current_fiber_id,
    yield_execution,
    num_workers,
    _HAS_CYTHON,
    # Worker management
    check_worker_scaling,
    set_auto_scaling,
    set_energy_efficient_mode,
    get_worker_utilization,
    get_recommended_workers,
)

# Import task/sync model
from .task import (
    task,
    sync,
    sync_timeout,
    task_count,
    run,
    task_batch,
    task_fast,
    task_batch_fast,
)

# Import async/await model
from .async_ import (
    create_task,
    sleep,
    gather,
    wait_for,
    ensure_future,
    async_range,
    AsyncIterator,
    AsyncRange,
    AsyncContextManager,
    create_tcp_socket,
    create_udp_socket,
    has_native_io,
)

# Import native I/O
from .native_io import (
    NativeSocket,
    NativeEventLoop,
    _HAS_NATIVE_IO,
)

# Import channel operations
from .channel import (
    Chan,
    chan,
    send,
    recv,
    close,
)

# Import waitgroup operations
from .waitgroup import (
    WaitGroup,
    create_wg,
    add,
    done,
    wait,
)

# Import select operations
from .select import (
    SelectCase,
    SelectResult,
    recv as select_recv,
    send as select_send,
    default,
    select,
)

# Import future
from .future import (
    Future as _Future,
    ensure_future as _ensure_future,
    is_future,
)

# Re-export with consistent names
Future = _Future
ensure_future = _ensure_future


# Public API
__all__ = [
    # Version
    '__version__',
    '__author__',
    
    # Core
    'Future',
    'Channel',
    'WaitGroup',
    'init_scheduler',
    'shutdown_scheduler',
    'get_scheduler_stats',
    'spawn',
    'spawn_batch',
    'spawn_batch_fast',
    'sleep_ns',
    'sleep_us',
    'sleep_ms',
    'current_fiber_id',
    'yield_execution',
    'num_workers',
    '_HAS_CYTHON',
    
    # Worker management
    'check_worker_scaling',
    'set_auto_scaling',
    'set_energy_efficient_mode',
    'get_worker_utilization',
    'get_recommended_workers',
    
    # Task/Sync model
    'task',
    'sync',
    'sync_timeout',
    'task_count',
    'run',
    'task_batch',
    'task_fast',
    'task_batch_fast',
    
    # Async/Await model
    'create_task',
    'sleep',
    'gather',
    'wait_for',
    'ensure_future',
    'async_range',
    'AsyncIterator',
    'AsyncRange',
    'AsyncContextManager',
    'create_tcp_socket',
    'create_udp_socket',
    'has_native_io',

    # Native I/O
    'NativeSocket',
    'NativeEventLoop',
    '_HAS_NATIVE_IO',
    
    # Channel operations
    'Chan',
    'chan',
    'send',
    'recv',
    'close',
    
    # WaitGroup operations
    'create_wg',
    'add',
    'done',
    'wait',
    
    # Select operations
    'SelectCase',
    'SelectResult',
    'select_recv',
    'select_send',
    'default',
    'select',
    
    # Future utilities
    'is_future',
]
