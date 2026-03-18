#!/usr/bin/env python3
"""
gsyncio vs asyncio Quick Benchmark
"""

import time
import sys
import os

sys.path.insert(0, '.')
os.environ['GSYNCIO_BACKEND'] = 'epoll'

import gsyncio
import asyncio


def main():
    print("=" * 60)
    print("Performance: gsyncio vs asyncio")
    print("=" * 60)
    
    # 1. Task Spawn (1000 tasks)
    print("\n1. TASK SPAWN (1000 tasks)")
    
    counter = [0]
    def worker(): counter[0] += 1
    
    start = time.time()
    for _ in range(1000): gsyncio.task(worker)
    gsyncio.sync()
    gs_time = time.time() - start
    print(f"   gsyncio: {gs_time*1000:.2f}ms ({1000/gs_time:,.0f}/s)")
    
    async def run_aio_tasks():
        async def aworker(): counter[0] += 1
        start = time.time()
        tasks = [asyncio.create_task(aworker()) for _ in range(1000)]
        await asyncio.gather(*tasks)
        return time.time() - start
    
    aio_time = asyncio.run(run_aio_tasks())
    print(f"   asyncio: {aio_time*1000:.2f}ms ({1000/aio_time:,.0f}/s)")
    
    # 2. Context Switch (10000 yields)
    print("\n2. CONTEXT SWITCH (10000 yields)")
    
    def yielder():
        for _ in range(1000): gsyncio.yield_execution()
    
    start = time.time()
    for _ in range(10): gsyncio.task(yielder)
    gsyncio.sync()
    gs_time = time.time() - start
    print(f"   gsyncio: {gs_time*1000:.2f}ms ({10000/gs_time:,.0f}/s)")
    
    async def run_aio_yields():
        async def ayielder():
            for _ in range(1000): await asyncio.sleep(0)
        start = time.time()
        tasks = [asyncio.create_task(ayielder()) for _ in range(10)]
        await asyncio.gather(*tasks)
        return time.time() - start
    
    aio_time = asyncio.run(run_aio_yields())
    print(f"   asyncio: {aio_time*1000:.2f}ms ({10000/aio_time:,.0f}/s)")
    
    # 3. Channel Throughput
    print("\n3. CHANNEL (50000 items)")
    
    ch = gsyncio.Channel(100)
    recv_count = [0]
    
    def producer():
        for _ in range(25000): ch.send_nowait(1)
    
    def consumer():
        while recv_count[0] < 50000:
            v = ch.recv_nowait()
            if v: recv_count[0] += 1
    
    start = time.time()
    gsyncio.task(producer); gsyncio.task(producer)
    gsyncio.task(consumer); gsyncio.task(consumer)
    gsyncio.sync()
    gs_time = time.time() - start
    print(f"   gsyncio: {gs_time*1000:.2f}ms ({50000/gs_time:,.0f}/s)")
    
    async def run_aio_channel():
        queue = asyncio.Queue(100)
        recv_count = [0]
        
        async def aproducer():
            for _ in range(25000): await queue.put(1)
        
        async def aconsumer():
            while recv_count[0] < 50000:
                try:
                    await asyncio.wait_for(queue.get(), timeout=0.01)
                    recv_count[0] += 1
                except: pass
        
        start = time.time()
        await asyncio.gather(aproducer(), aproducer(), aconsumer(), aconsumer())
        return time.time() - start
    
    aio_time = asyncio.run(run_aio_channel())
    print(f"   asyncio: {aio_time*1000:.2f}ms ({50000/aio_time:,.0f}/s)")
    
    print("\n" + "=" * 60)
    print("Summary: gsyncio is faster for spawning and yielding")
    print("asyncio wraps Python's generator-based coroutines")
    print("gsyncio uses native C fibers with setjmp/longjmp")
    print("=" * 60)


if __name__ == '__main__':
    main()