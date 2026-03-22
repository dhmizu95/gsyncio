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
    Task,
    create_task,
    run,
    get_current_loop,
    set_current_loop,
    fiber_park,
    is_future,
    sleep,
)

# Import channel operations
from .channel import (
    Chan,
    chan,
)

# Import select operations
from .select import (
    select,
    recv,
    send,
    default,
    SelectCase,
    SelectResult,
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
    'Task',
    'create_task',
    'get_current_loop',
    'set_current_loop',
    'fiber_park',
    'is_future',
    
    # Channel operations
    'Chan',
    'chan',
    'create_chan',
    'send',
    'recv',
    'close',
]
