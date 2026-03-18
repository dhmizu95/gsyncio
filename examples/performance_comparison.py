#!/usr/bin/env python3
"""
Performance Comparison - gsyncio vs asyncio vs threading

This example compares the performance of gsyncio, asyncio, and
threading for concurrent I/O-bound operations.
"""

import time
import asyncio
import threading


def benchmark_gsyncio(num_tasks=1000, delay_ms=1):
    """Benchmark gsyncio"""
    import gsyncio as gs
    import asyncio
    
    async def task(n):
        await gs.sleep(delay_ms)
        return n
    
    async def main():
        tasks = [asyncio.create_task(task(i)) for i in range(num_tasks)]
        results = await asyncio.gather(*tasks)
        return len(results)
    
    start = time.time()
    from gsyncio.async_ import run as async_run
    async_run(main())
    elapsed = time.time() - start
    
    return elapsed, num_tasks


def benchmark_asyncio(num_tasks=1000, delay_ms=1):
    """Benchmark asyncio"""
    async def task(n):
        await asyncio.sleep(delay_ms / 1000.0)
        return n
    
    async def main():
        tasks = [asyncio.create_task(task(i)) for i in range(num_tasks)]
        results = await asyncio.gather(*tasks)
        return len(results)
    
    start = time.time()
    result = asyncio.run(main())
    elapsed = time.time() - start
    
    return elapsed, result


def benchmark_threading(num_tasks=100, delay_ms=1):
    """Benchmark threading (limited due to overhead)"""
    results = []
    lock = threading.Lock()
    
    def task(n):
        time.sleep(delay_ms / 1000.0)
        with lock:
            results.append(n)
    
    threads = []
    start = time.time()
    
    for i in range(num_tasks):
        t = threading.Thread(target=task, args=(i,))
        threads.append(t)
        t.start()
    
    for t in threads:
        t.join()
    
    elapsed = time.time() - start
    
    return elapsed, len(results)


def main():
    """Run benchmarks"""
    print("Performance Comparison: gsyncio vs asyncio vs threading")
    print("=" * 60)
    
    num_tasks = 1000
    delay_ms = 1
    
    print(f"\nConfiguration: {num_tasks} tasks, {delay_ms}ms delay each")
    print("-" * 60)
    
    # Benchmark gsyncio
    print("\nBenchmarking gsyncio...")
    try:
        elapsed, result = benchmark_gsyncio(num_tasks, delay_ms)
        print(f"  gsyncio:   {elapsed*1000:.2f}ms ({result} tasks)")
    except Exception as e:
        print(f"  gsyncio:   Failed - {e}")
        elapsed = None
    
    # Benchmark asyncio
    print("\nBenchmarking asyncio...")
    try:
        elapsed_asyncio, result = benchmark_asyncio(num_tasks, delay_ms)
        print(f"  asyncio:   {elapsed_asyncio*1000:.2f}ms ({result} tasks)")
    except Exception as e:
        print(f"  asyncio:   Failed - {e}")
        elapsed_asyncio = None
    
    # Benchmark threading (with fewer tasks due to overhead)
    print("\nBenchmarking threading (100 tasks)...")
    try:
        elapsed_thread, result = benchmark_threading(100, delay_ms)
        print(f"  threading: {elapsed_thread*1000:.2f}ms ({result} tasks)")
    except Exception as e:
        print(f"  threading: Failed - {e}")
        elapsed_thread = None
    
    # Summary
    print("\n" + "=" * 60)
    print("Summary:")
    
    if elapsed and elapsed_asyncio:
        speedup = elapsed_asyncio / elapsed if elapsed > 0 else float('inf')
        print(f"  gsyncio vs asyncio: {speedup:.2f}x {'faster' if speedup > 1 else 'slower'}")
    
    print("\nNote: Actual performance depends on workload and system.")
    print("gsyncio excels with high-concurrency I/O-bound workloads.")


if __name__ == '__main__':
    main()
