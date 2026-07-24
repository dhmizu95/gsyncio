import gsyncio.core as core
import time

async def worker(n):
    print(f"Worker {n} starting")
    # In Phase 5 we will have a proper sleep. For now, just yield a few times.
    # Note: Our loop_run currently only handles futures and basic yields.
    # Since we don't have a way to yield yet from Python (except via await future),
    # let's just test if it runs.
    print(f"Worker {n} done")

def test_basic_loop():
    print("Testing basic loop...")
    from gsyncio._gsyncio_core import loop_add_ready, loop_set_main_fiber
    
    loop = core.EventLoop()
    
    coro = worker(1)
    fiber = core.Fiber(coro)
    
    # Set this as main fiber so loop knows when to stop
    loop_set_main_fiber(loop._loop, fiber._fiber)
    # Add to ready queue to start execution
    loop_add_ready(loop._loop, fiber._fiber)
    
    print("Running loop...")
    loop.run()
    print("Loop finished.")

if __name__ == "__main__":
    test_basic_loop()
