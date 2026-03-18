#!/usr/bin/env python3
"""
gsyncio Performance Benchmark - Comparing with Go coroutines
"""

import time
import sys
sys.path.insert(0, '..')

import gsyncio


def benchmark_task_spawn(num_tasks=1000):
    """Benchmark task spawning overhead"""
    print(f"\n=== Task Spawn Benchmark ({num_tasks} tasks) ===")

    counter = [0]

    def worker():
        counter[0] += 1

    def main():
        start = time.time()
        for _ in range(num_tasks):
            gsyncio.task(worker)
        gsyncio.sync()
        elapsed = time.time() - start
        return elapsed

    gsyncio.init_scheduler()
    elapsed = main()
    gsyncio.shutdown_scheduler(wait=True)

    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks/sec: {num_tasks/elapsed:,.0f}")
    print(f"  Per task: {elapsed*1000000/num_tasks:.2f}µs")
    print(f"  Completed: {counter[0]}/{num_tasks}")
    return elapsed


def benchmark_gsyncio_sleep(num_tasks=100):
    """Benchmark gsyncio sleep with goroutine-like behavior"""
    print(f"\n=== gsyncio Sleep Benchmark ({num_tasks} tasks) ===")
    
    def task():
        gsyncio.sleep_ms(1)  # Sleep 1ms
    
    def main():
        start = time.time()
        for _ in range(num_tasks):
            gsyncio.task(task)
        gsyncio.sync()
        elapsed = time.time() - start
        return elapsed
    
    gsyncio.init_scheduler()
    elapsed = main()
    gsyncio.shutdown_scheduler(wait=True)
    
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Tasks: {num_tasks}")
    return elapsed


def benchmark_waitgroup(num_workers=10):
    """Benchmark WaitGroup synchronization"""
    print(f"\n=== WaitGroup Benchmark ({num_workers} workers) ===")

    def run_benchmark():
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
        return elapsed, counter[0]

    gsyncio.init_scheduler()
    elapsed, count = run_benchmark()
    gsyncio.shutdown_scheduler(wait=True)

    total_ops = num_workers * 100
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Operations: {total_ops}")
    print(f"  Ops/sec: {total_ops/elapsed:,.0f}")
    return elapsed


def benchmark_context_switch(num_yields=10000):
    """Benchmark context switching via yield"""
    print(f"\n=== Context Switch Benchmark ({num_yields} yields) ===")

    def run_benchmark():
        def yielder():
            for _ in range(num_yields // 10):
                gsyncio.yield_execution()

        start = time.time()
        for _ in range(10):
            gsyncio.task(yielder)
        gsyncio.sync()
        elapsed = time.time() - start
        return elapsed

    gsyncio.init_scheduler()
    elapsed = run_benchmark()
    gsyncio.shutdown_scheduler(wait=True)
    
    print(f"  Total time: {elapsed*1000:.2f}ms")
    print(f"  Yields/sec: {num_yields/elapsed:,.0f}")
    print(f"  Per yield: {elapsed*1000000/num_yields:.2f}µs")
    return elapsed


def main():
    """Run all benchmarks"""
    print("=" * 60)
    print("gsyncio Performance Benchmark vs Go")
    print("=" * 60)
    print(f"Python: {sys.version.split()[0]}")
    print(f"sys.path: {sys.path}")
    
    # Import gsyncio
    sys.path.insert(0, '..')
    import gsyncio
    print(f"gsyncio.__file__: {gsyncio.__file__}")
    print(f"C Extension: {gsyncio._HAS_CYTHON}")
    print(f"CPU Cores: {gsyncio.num_workers()}")
    print()
    
    results = {}
    
    results['task_spawn'] = benchmark_task_spawn(1000)
    results['sleep'] = benchmark_gsyncio_sleep(100)
    results['waitgroup'] = benchmark_waitgroup(10)
    results['context_switch'] = benchmark_context_switch(10000)
    
    # Summary
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"Task spawn (1000):        {results['task_spawn']*1000:.2f}ms")
    print(f"Sleep (100):              {results['sleep']*1000:.2f}ms")
    print(f"WaitGroup (10×100):       {results['waitgroup']*1000:.2f}ms")
    print(f"Context switch (10000):   {results['context_switch']*1000:.2f}ms")
    
    print("\nNote: gsyncio uses threading-based concurrency in pure Python mode.")
    print("Go uses M:N goroutine scheduling with segmented stacks.")
    print("\nTo run Go benchmark: go run benchmark_go.go")


if __name__ == '__main__':
    main()
