#!/usr/bin/env python3
"""
Quick spawn performance benchmark - gsyncio vs asyncio
"""

import time
import sys

def benchmark_gsyncio_spawn(num_tasks=10000):
    """Benchmark gsyncio task spawn (high-level API)"""
    import gsyncio
    
    # Pre-initialize scheduler
    gsyncio.init_scheduler(num_workers=4)
    
    counter = 0
    
    def worker():
        nonlocal counter
        counter += 1
    
    start = time.perf_counter()
    for _ in range(num_tasks):
        gsyncio.task(worker)
    gsyncio.sync()
    elapsed = time.perf_counter() - start
    
    gsyncio.shutdown_scheduler(wait=True)
    return elapsed, counter


def benchmark_asyncio_spawn(num_tasks=10000):
    """Benchmark asyncio task spawn"""
    import asyncio
    
    counter = 0
    
    async def worker():
        nonlocal counter
        counter += 1
    
    async def main():
        nonlocal counter
        start = time.perf_counter()
        tasks = [asyncio.create_task(worker()) for _ in range(num_tasks)]
        await asyncio.gather(*tasks)
        elapsed = time.perf_counter() - start
        return elapsed, counter
    
    return asyncio.run(main())


def benchmark_gsyncio_raw_spawn(num_tasks=10000):
    """Benchmark gsyncio raw spawn (direct C call)"""
    import gsyncio
    
    # Pre-initialize scheduler
    gsyncio.init_scheduler(num_workers=4)
    
    counter = 0
    
    def worker():
        nonlocal counter
        counter += 1
    
    start = time.perf_counter()
    for _ in range(num_tasks):
        gsyncio.spawn(worker)
    gsyncio.sync()
    elapsed = time.perf_counter() - start
    
    gsyncio.shutdown_scheduler(wait=True)
    return elapsed, counter


def benchmark_gsyncio_task_fast(num_tasks=10000):
    """Benchmark gsyncio task_fast (ultra-fast path)"""
    import gsyncio
    
    # Pre-initialize scheduler
    gsyncio.init_scheduler(num_workers=4)
    
    counter = 0
    
    def worker():
        nonlocal counter
        counter += 1
    
    start = time.perf_counter()
    for _ in range(num_tasks):
        gsyncio.task_fast(worker)
    # Note: task_fast doesn't support sync()
    elapsed = time.perf_counter() - start
    
    gsyncio.shutdown_scheduler(wait=True)
    return elapsed, counter


def main():
    print("=" * 70)
    print("Spawn Performance Benchmark")
    print("=" * 70)
    
    # Test with different task counts
    for num_tasks in [1000, 5000, 10000]:
        print(f"\n--- {num_tasks} tasks ---")
        
        # gsyncio high-level (task/sync)
        elapsed_gs, count_gs = benchmark_gsyncio_spawn(num_tasks)
        rate_gs = num_tasks / elapsed_gs
        print(f"gsyncio.task():    {elapsed_gs*1000:7.2f}ms  | {rate_gs:12,.0f}/s  | completed: {count_gs}")
        
        # gsyncio raw spawn
        elapsed_raw, count_raw = benchmark_gsyncio_raw_spawn(num_tasks)
        rate_raw = num_tasks / elapsed_raw
        print(f"gsyncio.spawn():   {elapsed_raw*1000:7.2f}ms  | {rate_raw:12,.0f}/s  | completed: {count_raw}")
        
        # gsyncio task_fast
        elapsed_fast, _ = benchmark_gsyncio_task_fast(num_tasks)
        rate_fast = num_tasks / elapsed_fast
        print(f"gsyncio.task_fast(): {elapsed_fast*1000:7.2f}ms  | {rate_fast:12,.0f}/s")
        
        # asyncio
        elapsed_as, count_as = benchmark_asyncio_spawn(num_tasks)
        rate_as = num_tasks / elapsed_as
        print(f"asyncio:           {elapsed_as*1000:7.2f}ms  | {rate_as:12,.0f}/s  | completed: {count_as}")
        
        # Comparison
        print(f"  → gsyncio.task() is {elapsed_gs/elapsed_as:.2f}x vs asyncio")
        print(f"  → gsyncio.spawn() is {elapsed_raw/elapsed_as:.2f}x vs asyncio ({'FASTER' if elapsed_raw < elapsed_as else 'slower'})")
        print(f"  → gsyncio.task_fast() is {elapsed_fast/elapsed_as:.2f}x vs asyncio")
    
    print("\n" + "=" * 70)
    print("Analysis:")
    print("=" * 70)
    print("""
Key observations:
1. gsyncio.task() has overhead from Python wrapper (locking, counting)
2. gsyncio.spawn() is the raw C extension call - often FASTER than asyncio
3. gsyncio.task_fast() provides minimal-overhead spawning
4. asyncio is highly optimized for single-threaded coroutine spawning

Recommendations:
- Use task() when you need sync() support
- Use task_fast() for maximum spawn rate (fire-and-forget)
- Use spawn() directly for lowest-level control
- Use task_batch() for spawning many tasks at once
""")


if __name__ == '__main__':
    main()
