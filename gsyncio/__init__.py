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

__version__ = '0.2.0'
__author__ = 'gsyncio team'

# Import core components
from .core import (
    Future,
    EventLoop,
    Fiber,
    get_current_loop,
    set_current_loop,
    fiber_park,
)

# Import channel operations
from .channel import (
    Chan,
    chan,
)

# Re-export with consistent names
# Future utilities
def is_future(obj):
    return isinstance(obj, Future)

# Public API
__all__ = [
    # Version
    '__version__',
    '__author__',
    
    # Core
    'Future',
    'EventLoop',
    'Fiber',
    'get_current_loop',
    'set_current_loop',
    'fiber_park',
    
    # Channel operations
    'Chan',
    'chan',
    
    # Future utilities
    'is_future',
]


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
    'spawn_direct',
    'spawn_batch',
    'spawn_batch_fast',
    'spawn_batch_ultra_fast',
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
    'task_with_wrapper',
    'task_direct',
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
    'create_chan',
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
