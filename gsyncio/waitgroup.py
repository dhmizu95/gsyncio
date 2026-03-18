"""
gsyncio.waitgroup - WaitGroup synchronization primitive

This module provides a WaitGroup for waiting on multiple concurrent
operations to complete (like Go's sync.WaitGroup).

Usage:
    import gsyncio as gs
    
    async def worker(wg, worker_id):
        try:
            await gs.sleep(100)
            print(f"Worker {worker_id} completed")
        finally:
            gs.done(wg)
    
    async def main():
        wg = gs.create_wg()
        
        # Add 5 workers
        gs.add(wg, 5)
        
        # Spawn workers
        for i in range(5):
            gs.task(worker, wg, i)
        
        # Wait for all workers
        await gs.wait(wg)
        print("All workers completed")
    
    gs.run(main)
"""

from .core import WaitGroup as _WaitGroup


class WaitGroup:
    """
    WaitGroup for synchronizing on multiple concurrent operations.
    """
    
    def __init__(self):
        self._wg = _WaitGroup()
    
    def add(self, delta: int = 1) -> None:
        """
        Add delta to the counter.
        
        Must be called before starting the operation(s).
        
        Args:
            delta: Value to add (default 1)
        
        Raises:
            RuntimeError: If counter would become negative
        """
        self._wg.add(delta)
    
    def done(self) -> None:
        """
        Decrement the counter by 1.
        
        Should be called when an operation completes, typically
        in a defer/finally block.
        """
        self._wg.done()
    
    async def wait(self) -> None:
        """
        Wait for the counter to reach zero.
        
        Blocks until all operations have called done().
        """
        await self._wg.wait()
    
    @property
    def counter(self) -> int:
        """Current counter value"""
        return self._wg.counter


def create_wg() -> WaitGroup:
    """
    Create a new WaitGroup.
    
    Returns:
        New WaitGroup
    """
    return WaitGroup()


def add(wg: WaitGroup, delta: int = 1) -> None:
    """
    Add delta to a WaitGroup counter.
    
    Args:
        wg: WaitGroup
        delta: Value to add (default 1)
    """
    wg.add(delta)


def done(wg: WaitGroup) -> None:
    """
    Decrement a WaitGroup counter.
    
    Args:
        wg: WaitGroup
    """
    wg.done()


async def wait(wg: WaitGroup) -> None:
    """
    Wait for a WaitGroup counter to reach zero.
    
    Args:
        wg: WaitGroup
    """
    await wg.wait()


__all__ = ['WaitGroup', 'create_wg', 'add', 'done', 'wait']
