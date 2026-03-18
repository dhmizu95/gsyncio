#!/usr/bin/env python3
"""
gsyncio vs Go vs asyncio - Complete Performance Comparison
"""

import time
import sys
import threading

sys.path.insert(0, '..')
import gsyncio as gs


def benchmark_gsyncio_task_spawn(num_tasks=1000):
    """Benchmark gsyncio task spawning overhead"""
    print(f"\n=== gsyncio Task Spawn Benchmark ({num_tasks} tasks) ===")

    counter = [0]
    lock = threading.Lock()

    def worker():
        with lock:
            counter[0] += 1

    gs.init_scheduler(num_workers=4)
    start = time.time()
    for _ in range(num_tasks):
        gs.task(worker)
    gs.sync()
    elapsed = time.time() - start
    gs.shutdown_scheduler()

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks/sec: {num_tasks/elapsed:,.0f}")
    print(f"  Per task: {elapsed*1000000/num_tasks:.2f}µs")
    print(f"  Completed: {counter[0]}/{num_tasks}")
    return elapsed


def benchmark_gsyncio_sleep(num_tasks=100):
    """Benchmark gsyncio sleep"""
    print(f"\n=== gsyncio Sleep Benchmark ({num_tasks} tasks) ===")

    def task():
        gs.sleep_ms(1)

    gs.init_scheduler(num_workers=4)
    start = time.time()
    for _ in range(num_tasks):
        gs.task(task)
    gs.sync()
    elapsed = time.time() - start
    gs.shutdown_scheduler()

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks: {num_tasks}")
    return elapsed


def benchmark_gsyncio_waitgroup(num_workers=10):
    """Benchmark gsyncio WaitGroup synchronization"""
    print(f"\n=== gsyncio WaitGroup Benchmark ({num_workers} workers) ===")

    counter = [0]
    lock = threading.Lock()
    wg = gs.WaitGroup()

    def worker():
        for _ in range(100):
            with lock:
                counter[0] += 1
        gs.done(wg)

    gs.init_scheduler(num_workers=4)
    gs.add(wg, num_workers)
    start = time.time()
    for _ in range(num_workers):
        gs.task(worker)
    gs.sync()
    elapsed = time.time() - start
    gs.shutdown_scheduler()

    total_ops = num_workers * 100
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Operations: {total_ops}")
    print(f"  Ops/sec: {total_ops/elapsed:,.0f}")
    return elapsed


def benchmark_gsyncio_context_switch(num_yields=10000):
    """Benchmark gsyncio context switching via yield"""
    print(f"\n=== gsyncio Context Switch Benchmark ({num_yields} yields) ===")

    def yielder():
        for _ in range(num_yields // 10):
            gs.yield_execution()

    gs.init_scheduler(num_workers=4)
    start = time.time()
    for _ in range(10):
        gs.task(yielder)
    gs.sync()
    elapsed = time.time() - start
    gs.shutdown_scheduler()

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Yields/sec: {num_yields/elapsed:,.0f}")
    print(f"  Per yield: {elapsed*1000000/num_yields:.2f}µs")
    return elapsed


def main():
    """Run all gsyncio benchmarks"""
    print("=" * 60)
    print("gsyncio Performance Benchmark (GIL removed, C implementation)")
    print("=" * 60)
    print(f"Python: {sys.version.split()[0]}")
    print(f"C Extension: {gs._HAS_CYTHON}")
    print(f"CPU Cores: {gs.num_workers()}")
    print()

    results = {}

    results['task_spawn'] = benchmark_gsyncio_task_spawn(1000)
    results['sleep'] = benchmark_gsyncio_sleep(100)
    results['waitgroup'] = benchmark_gsyncio_waitgroup(10)
    results['context_switch'] = benchmark_gsyncio_context_switch(10000)

    # Summary
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"Task spawn (1000):        {results['task_spawn']*1000:.2f}ms")
    print(f"Sleep (100):              {results['sleep']*1000:.2f}ms")
    print(f"WaitGroup (10×100):       {results['waitgroup']*1000:.2f}ms")
    print(f"Context switch (10000):   {results['context_switch']*1000:.2f}ms")


if __name__ == '__main__':
    main()
