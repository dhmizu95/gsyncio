#!/usr/bin/env python3
"""
Benchmark: C Tasks vs Python Tasks

This benchmark demonstrates the performance difference between:
1. C tasks (GIL-free, true parallelism)
2. Python tasks (GIL-bound, serialized execution)

Expected Results:
    C tasks should be 50-100x faster for CPU-bound workloads
    because they bypass the GIL and run in true parallel.
"""

import time
import sys
sys.path.insert(0, '.')

import gsyncio
from gsyncio import c_tasks

def benchmark_c_tasks(num_tasks=1000, n=100000):
    """Benchmark C task execution (GIL-free)"""
    print(f"\n{'='*70}")
    print(f"C TASKS BENCHMARK ({num_tasks} tasks, n={n})")
    print(f"{'='*70}")
    print("C tasks execute WITHOUT the GIL - true parallel execution!")
    
    c_tasks.reset_stats()
    
    start = time.time()
    
    # Spawn C tasks (GIL-free!)
    for i in range(num_tasks):
        c_tasks.spawn_sum_squares(n)
    
    spawn_time = time.time() - start
    print(f"Spawn time: {spawn_time*1000:.2f}ms")
    
    # Wait for completion
    gsyncio.sync()
    
    total_time = time.time() - start
    stats = c_tasks.get_stats()
    
    print(f"Total time: {total_time*1000:.2f}ms")
    print(f"Tasks/sec: {num_tasks/total_time:,.0f}")
    print(f"Per task: {total_time*1000000/num_tasks:.2f}µs")
    print(f"C tasks completed: {stats['c_tasks_completed']}")
    print(f"Python tasks completed: {stats['python_tasks_completed']}")
    
    return total_time

def benchmark_python_tasks(num_tasks=1000, n=100000):
    """Benchmark Python task execution (GIL-bound)"""
    print(f"\n{'='*70}")
    print(f"PYTHON TASKS BENCHMARK ({num_tasks} tasks, n={n})")
    print(f"{'='*70}")
    print("Python tasks require the GIL - serialized execution!")
    
    counter = [0]
    
    def python_sum_squares(n):
        """Python version of sum_squares"""
        total = 0
        for i in range(n):
            total += i * i
        counter[0] += 1
    
    start = time.time()
    
    # Spawn Python tasks (GIL-bound!)
    for i in range(num_tasks):
        gsyncio.spawn(python_sum_squares, n)
    
    spawn_time = time.time() - start
    print(f"Spawn time: {spawn_time*1000:.2f}ms")
    
    # Wait for completion
    gsyncio.sync()
    
    total_time = time.time() - start
    
    print(f"Total time: {total_time*1000:.2f}ms")
    print(f"Tasks/sec: {num_tasks/total_time:,.0f}")
    print(f"Per task: {total_time*1000000/num_tasks:.2f}µs")
    print(f"Python tasks completed: {counter[0]}")
    
    return total_time

def main():
    print("="*70)
    print("GSYNCIO: C TASKS vs PYTHON TASKS BENCHMARK")
    print("="*70)
    print(f"Python: {sys.version.split()[0]}")
    print(f"CPU Cores: {gsyncio.num_workers()}")
    print()
    
    # Initialize scheduler
    gsyncio.init_scheduler(num_workers=4)
    print(f"Scheduler initialized with {gsyncio.num_workers()} workers")
    
    num_tasks = 100
    n = 100000
    
    # Benchmark C tasks
    c_time = benchmark_c_tasks(num_tasks, n)
    
    # Benchmark Python tasks
    py_time = benchmark_python_tasks(num_tasks, n)
    
    # Summary
    print(f"\n{'='*70}")
    print("SUMMARY")
    print(f"{'='*70}")
    print(f"C Tasks:       {c_time*1000:7.2f}ms ({num_tasks/c_time:10,.0f}/s)")
    print(f"Python Tasks:  {py_time*1000:7.2f}ms ({num_tasks/py_time:10,.0f}/s)")
    print(f"Speedup:       {py_time/c_time:.1f}x faster with C tasks!")
    print()
    print("WHY C TASKS ARE FASTER:")
    print("  • C tasks: NO GIL - all 4 workers run in parallel")
    print("  • Python tasks: GIL contention - only 1 worker runs at a time")
    print("  • C tasks: Pure C computation - no Python overhead")
    print("  • Python tasks: Python interpreter overhead")
    print(f"{'='*70}")
    
    gsyncio.shutdown_scheduler(wait=False)
    print("\n✅ Benchmark complete!\n")

if __name__ == '__main__':
    main()
