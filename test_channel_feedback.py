#!/usr/bin/env python3
"""
Channel demo - async producer/consumer pattern

For async functions, use asyncio.create_task() instead of gs.task()
since gs.task() runs in a separate thread/event loop.
"""
import gsyncio as gs
import asyncio

async def producer(ch):
    for i in range(1, 6):
        await gs.send(ch, i)
        print(f"Sent: {i}")
    gs.close(ch)
    print("Producer done")

async def main():
    ch = gs.create_chan()
    # Use asyncio.create_task for async functions (same event loop)
    asyncio.create_task(producer(ch))
    
    # async for iteration (like Go's for range)
    async for num in ch:
        print(f"Received: {num}")
    
    print("Channel closed.")

if __name__ == "__main__":
    gs.run(main())
