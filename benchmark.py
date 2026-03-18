#!/usr/bin/env python3
"""
gsyncio Performance Benchmark - Simple Version
"""

import time
import sys
sys.path.insert(0, '.')

import gsyncio
import asyncio


def benchmark_task_spawn(num_tasks=1000):
    """Benchmark task spawning overhead"""
    print(f"\n=== Task Spawn Benchmark ({num_tasks} tasks) ===")
    
    counter = [0]
    
    def worker():
        counter[0] += 1
    
    start = time.time()
    for _ in range(num_tasks):
        gsyncio.task(worker)
    gsyncio.sync()
    elapsed = time.time() - start
    
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks/sec: {num_tasks/elapsed:,.0f}")
    print(f"  Per task: {elapsed*1000000/num_tasks:.2f}µs")
    print(f"  Completed: {counter[0]}/{num_tasks}")
    return elapsed


def benchmark_async_gather(num_tasks=100):
    """Benchmark async gather with sleep"""
    print(f"\n=== Async Gather Benchmark ({num_tasks} tasks) ===")
    
    async def task():
        await gsyncio.sleep(1)
    
    async def main():
        tasks = [asyncio.create_task(task()) for _ in range(num_tasks)]
        await asyncio.gather(*tasks)
    
    start = time.time()
    asyncio.run(main())
    elapsed = time.time() - start
    
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks: {num_tasks}")
    return elapsed


def benchmark_waitgroup(num_workers=10):
    """Benchmark WaitGroup synchronization"""
    print(f"\n=== WaitGroup Benchmark ({num_workers} workers) ===")
    
    wg = gsyncio.create_wg()
    counter = [0]
    
    def worker():
        for _ in range(100):
            counter[0] += 1
        gsyncio.done(wg)
    
    gsyncio.add(wg, num_workers)
    
    start = time.time()
    for _ in range(num_workers):
        gsyncio.task(worker)
    gsyncio.sync()
    elapsed = time.time() - start
    
    total_ops = num_workers * 100
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Operations: {total_ops}")
    print(f"  Ops/sec: {total_ops/elapsed:,.0f}")
    return elapsed


def benchmark_context_switch(num_yields=10000):
    """Benchmark context switching via yield"""
    print(f"\n=== Context Switch Benchmark ({num_yields} yields) ===")
    
    def yielder():
        for _ in range(num_yields // 10):
            gsyncio.yield_execution()
    
    start = time.time()
    for _ in range(10):
        gsyncio.task(yielder)
    gsyncio.sync()
    elapsed = time.time() - start
    
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Yields/sec: {num_yields/elapsed:,.0f}")
    print(f"  Per yield: {elapsed*1000000/num_yields:.2f}µs")
    return elapsed


def compare_asyncio(num_tasks=100):
    """Compare with pure asyncio"""
    print(f"\n=== asyncio Comparison ({num_tasks} tasks) ===")
    
    # asyncio
    async def aio_task():
        await asyncio.sleep(0.001)
    
    async def aio_main():
        tasks = [asyncio.create_task(aio_task()) for _ in range(num_tasks)]
        await asyncio.gather(*tasks)
    
    start = time.time()
    asyncio.run(aio_main())
    aio_elapsed = time.time() - start
    
    # gsyncio (wraps asyncio for async operations)
    async def gs_task():
        await gsyncio.sleep(1)
    
    async def gs_main():
        tasks = [asyncio.create_task(gs_task()) for _ in range(num_tasks)]
        await asyncio.gather(*tasks)
    
    start = time.time()
    asyncio.run(gs_main())
    gs_elapsed = time.time() - start
    
    print(f"  asyncio: {aio_elapsed*1000:.2f}ms")
    print(f"  gsyncio: {gs_elapsed*1000:.2f}ms")
    print(f"  Ratio:   {gs_elapsed/aio_elapsed:.2f}x")
    
    return aio_elapsed, gs_elapsed


def main():
    """Run all benchmarks"""
    print("=" * 60)
    print("gsyncio Performance Benchmark")
    print("=" * 60)
    print(f"Python: {sys.version.split()[0]}")
    print(f"C Extension: {gsyncio._HAS_CYTHON}")
    print()
    
    results = {}
    
    results['task_spawn'] = benchmark_task_spawn(1000)
    results['async_gather'] = benchmark_async_gather(100)
    results['waitgroup'] = benchmark_waitgroup(10)
    results['context_switch'] = benchmark_context_switch(10000)
    results['comparison'] = compare_asyncio(100)
    
    # Summary
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"Task spawn (1000):        {results['task_spawn']*1000:.2f}ms")
    print(f"Async gather (100):       {results['async_gather']*1000:.2f}ms")
    print(f"WaitGroup (10×100):       {results['waitgroup']*1000:.2f}ms")
    print(f"Context switch (10000):   {results['context_switch']*1000:.2f}ms")
    
    aio, gs = results['comparison']
    print(f"\nasyncio vs gsyncio ratio: {gs/aio:.2f}x")
    
    print("\nNote: Current implementation wraps asyncio for async operations.")
    print("The C extension provides native fiber scheduling for task/spawn.")


if __name__ == '__main__':
    main()
