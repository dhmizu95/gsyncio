#!/usr/bin/env python3
"""
asyncio Performance Benchmark
"""

import asyncio
import time
import sys


async def benchmark_task_spawn_asyncio(num_tasks=1000):
    """Benchmark task spawning overhead with asyncio"""
    print(f"\n=== asyncio Task Spawn Benchmark ({num_tasks} tasks) ===")

    counter = 0

    async def worker():
        nonlocal counter
        counter += 1

    start = time.time()
    tasks = [asyncio.create_task(worker()) for _ in range(num_tasks)]
    await asyncio.gather(*tasks)
    elapsed = time.time() - start

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks/sec: {num_tasks/elapsed:,.0f}")
    print(f"  Per task: {elapsed*1000000/num_tasks:.2f}µs")
    print(f"  Completed: {counter}/{num_tasks}")
    return elapsed


async def benchmark_asyncio_sleep(num_tasks=100):
    """Benchmark asyncio sleep"""
    print(f"\n=== asyncio Sleep Benchmark ({num_tasks} tasks) ===")

    async def task():
        await asyncio.sleep(0.001)  # Sleep 1ms

    start = time.time()
    tasks = [asyncio.create_task(task()) for _ in range(num_tasks)]
    await asyncio.gather(*tasks)
    elapsed = time.time() - start

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks: {num_tasks}")
    return elapsed


async def benchmark_asyncio_waitgroup(num_workers=10):
    """Benchmark asyncio synchronization (using Event)"""
    print(f"\n=== asyncio WaitGroup Benchmark ({num_workers} workers) ===")

    counter = 0
    counter_lock = asyncio.Lock()
    events = [asyncio.Event() for _ in range(num_workers)]

    async def worker(event):
        nonlocal counter
        for _ in range(100):
            async with counter_lock:
                counter += 1
        event.set()

    start = time.time()
    tasks = [asyncio.create_task(worker(events[i])) for i in range(num_workers)]
    await asyncio.gather(*tasks)
    elapsed = time.time() - start

    total_ops = num_workers * 100
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Operations: {total_ops}")
    print(f"  Ops/sec: {total_ops/elapsed:,.0f}")
    return elapsed


async def benchmark_asyncio_context_switch(num_yields=10000):
    """Benchmark context switching via asyncio.sleep(0)"""
    print(f"\n=== asyncio Context Switch Benchmark ({num_yields} yields) ===")

    async def yielder():
        for _ in range(num_yields // 10):
            await asyncio.sleep(0)

    start = time.time()
    tasks = [asyncio.create_task(yielder()) for _ in range(10)]
    await asyncio.gather(*tasks)
    elapsed = time.time() - start

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Yields/sec: {num_yields/elapsed:,.0f}")
    print(f"  Per yield: {elapsed*1000000/num_yields:.2f}µs")
    return elapsed


async def main():
    """Run all asyncio benchmarks"""
    print("=" * 60)
    print("asyncio Performance Benchmark")
    print("=" * 60)
    print(f"Python: {sys.version.split()[0]}")
    print()

    results = {}

    results['task_spawn'] = await benchmark_task_spawn_asyncio(1000)
    results['sleep'] = await benchmark_asyncio_sleep(100)
    results['waitgroup'] = await benchmark_asyncio_waitgroup(10)
    results['context_switch'] = await benchmark_asyncio_context_switch(10000)

    # Summary
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"Task spawn (1000):        {results['task_spawn']*1000:.2f}ms")
    print(f"Sleep (100):              {results['sleep']*1000:.2f}ms")
    print(f"WaitGroup (10×100):       {results['waitgroup']*1000:.2f}ms")
    print(f"Context switch (10000):   {results['context_switch']*1000:.2f}ms")

    print("\nNote: asyncio uses single-threaded event loop with coroutines.")
    return results


if __name__ == '__main__':
    asyncio.run(main())
