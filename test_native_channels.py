import gsyncio as gs
import gsyncio.core as core
import time

async def producer(c):
    print("Producer: sending 1")
    await c.send(1)
    print("Producer: sending 2")
    await c.send(2)
    print("Producer: done")

async def consumer(c):
    print("Consumer: receiving...")
    v1 = await c.recv()
    print(f"Consumer: got {v1}")
    v2 = await c.recv()
    print(f"Consumer: got {v2}")
    print("Consumer: done")

def test_channels():
    print("Testing channels...")
    from gsyncio._gsyncio_core import loop_add_ready, loop_set_main_fiber
    
    loop = core.EventLoop()
    c = gs.chan(size=1) # Buffered
    
    p_coro = producer(c)
    p_fiber = core.Fiber(p_coro)
    
    c_coro = consumer(c)
    c_fiber = core.Fiber(c_coro)
    
    # We'll set the consumer as main fiber for this test
    # (In Phase 4 we'll have better ways to run multiple tasks)
    loop_set_main_fiber(loop._loop, c_fiber._fiber)
    
    # Add Consumer then Producer, so Producer is at head and runs first
    loop_add_ready(loop._loop, c_fiber._fiber)
    loop_add_ready(loop._loop, p_fiber._fiber)
    
    print("Running loop for channel test...")
    loop.run()
    print("Channel test loop finished.")

if __name__ == "__main__":
    test_channels()
