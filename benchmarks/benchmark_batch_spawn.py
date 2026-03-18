#!/usr/bin/env python3
"""
Benchmark: Batch Spawn Performance

This benchmark compares different batch spawning methods to demonstrate
the performance improvements from:
1. C-based task functions (no Python GIL needed during spawn)
2. Removing Python from the hot path entirely
3. Using Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS aggressively

Methods compared:
- spawn(): Individual task spawning (baseline)
- spawn_batch(): Original batch spawning (Python overhead)
- spawn_batch_ultra_fast(): New optimized batch spawning (GIL released)
"""

import time
import sys
from pathlib import Path

# Add parent directory to path
sys.path.insert(0, str(Path(__file__).parent.parent))

import gsyncio as gs


# Simple worker function for benchmarking
def worker(n):
    """Minimal worker - just compute a sum"""
    result = sum(range(n))
    return result


def benchmark_individual_spawn(count):
    """Benchmark individual spawn calls"""
    tasks = [(worker, (100,)) for _ in range(count)]
    
    start = time.perf_counter()
    for func, args in tasks:
        gs.spawn(func, *args)
    spawn_time = time.perf_counter() - start
    
    # Wait for completion
    gs.sync()
    
    return spawn_time


def benchmark_spawn_batch(count):
    """Benchmark original spawn_batch"""
    tasks = [(worker, (100,)) for _ in range(count)]
    
    start = time.perf_counter()
    gs.spawn_batch(tasks)
    spawn_time = time.perf_counter() - start
    
    # Wait for completion
    gs.sync()
    
    return spawn_time


def benchmark_spawn_batch_ultra_fast(count):
    """Benchmark new ultra-fast spawn with GIL release"""
    tasks = [(worker, (100,)) for _ in range(count)]
    
    start = time.perf_counter()
    gs.spawn_batch_ultra_fast(tasks)
    spawn_time = time.perf_counter() - start
    
    # Wait for completion
    gs.sync()
    
    return spawn_time


def run_benchmark(count, iterations=3):
    """Run benchmark with multiple iterations"""
    print(f"\n{'='*60}")
    print(f"Benchmarking with {count:,} tasks")
    print(f"{'='*60}")
    
    results = {
        'individual': [],
        'spawn_batch': [],
        'ultra_fast': [],
    }
    
    for i in range(iterations):
        print(f"\nIteration {i+1}/{iterations}")
        
        # Individual spawn
        gs.init_scheduler(num_workers=4)
        t = benchmark_individual_spawn(count)
        results['individual'].append(t)
        print(f"  Individual spawn: {t*1000:.2f} ms")
        gs.shutdown_scheduler(wait=True)
        
        # spawn_batch
        gs.init_scheduler(num_workers=4)
        t = benchmark_spawn_batch(count)
        results['spawn_batch'].append(t)
        print(f"  spawn_batch:      {t*1000:.2f} ms")
        gs.shutdown_scheduler(wait=True)
        
        # Ultra-fast batch
        gs.init_scheduler(num_workers=4)
        t = benchmark_spawn_batch_ultra_fast(count)
        results['ultra_fast'].append(t)
        print(f"  ultra_fast:       {t*1000:.2f} ms")
        gs.shutdown_scheduler(wait=True)
    
    # Calculate averages
    print(f"\n{'='*60}")
    print(f"Results (average of {iterations} iterations)")
    print(f"{'='*60}")
    
    avg_individual = sum(results['individual']) / len(results['individual'])
    avg_batch = sum(results['spawn_batch']) / len(results['spawn_batch'])
    avg_ultra = sum(results['ultra_fast']) / len(results['ultra_fast'])
    
    print(f"Individual spawn:  {avg_individual*1000:.2f} ms ({avg_individual*1000000/count:.2f} µs/task)")
    print(f"spawn_batch:       {avg_batch*1000:.2f} ms ({avg_batch*1000000/count:.2f} µs/task)")
    print(f"ultra_fast:        {avg_ultra*1000:.2f} ms ({avg_ultra*1000000/count:.2f} µs/task)")
    
    # Calculate speedup
    speedup_batch = avg_individual / avg_batch
    speedup_ultra = avg_individual / avg_ultra
    speedup_ultra_vs_batch = avg_batch / avg_ultra
    
    print(f"\nSpeedup vs individual:")
    print(f"  spawn_batch:  {speedup_batch:.2f}x faster")
    print(f"  ultra_fast:   {speedup_ultra:.2f}x faster")
    
    print(f"\nSpeedup ultra_fast vs spawn_batch:")
    print(f"  {speedup_ultra_vs_batch:.2f}x faster")
    
    # Tasks per second
    tasks_per_sec_individual = count / avg_individual
    tasks_per_sec_batch = count / avg_batch
    tasks_per_sec_ultra = count / avg_ultra
    
    print(f"\nThroughput:")
    print(f"  Individual spawn:  {tasks_per_sec_individual:,.0f} tasks/sec")
    print(f"  spawn_batch:       {tasks_per_sec_batch:,.0f} tasks/sec")
    print(f"  ultra_fast:        {tasks_per_sec_ultra:,.0f} tasks/sec")
    
    return {
        'individual': avg_individual,
        'spawn_batch': avg_batch,
        'ultra_fast': avg_ultra,
        'speedup_batch': speedup_batch,
        'speedup_ultra': speedup_ultra,
        'speedup_ultra_vs_batch': speedup_ultra_vs_batch,
    }


def main():
    print("="*60)
    print("Batch Spawn Performance Benchmark")
    print("="*60)
    print("\nComparing:")
    print("  1. Individual spawn() calls (baseline)")
    print("  2. spawn_batch() - Original batch spawn")
    print("  3. spawn_batch_ultra_fast() - Optimized with GIL release")
    
    # Test with different batch sizes
    test_sizes = [100, 1000, 5000, 10000]
    
    all_results = []
    for size in test_sizes:
        results = run_benchmark(size, iterations=3)
        all_results.append((size, results))
    
    # Summary table
    print(f"\n{'='*60}")
    print("SUMMARY TABLE")
    print(f"{'='*60}")
    print(f"{'Tasks':>10} | {'Individual':>12} | {'Batch':>12} | {'Ultra':>12} | {'Speedup':>10}")
    print(f"{'':>10} | {'(ms)':>12} | {'(ms)':>12} | {'(ms)':>12} | {'(vs ind)':>10}")
    print(f"{'-'*10}-+-{'-'*12}-+-{'-'*12}-+-{'-'*12}-+-{'-'*10}")
    
    for size, results in all_results:
        ind_ms = results['individual'] * 1000
        batch_ms = results['spawn_batch'] * 1000
        ultra_ms = results['ultra_fast'] * 1000
        speedup = results['speedup_ultra']
        print(f"{size:>10,} | {ind_ms:>12.2f} | {batch_ms:>12.2f} | {ultra_ms:>12.2f} | {speedup:>10.2f}x")
    
    print(f"\n{'='*60}")
    print("Key Optimizations in spawn_batch_ultra_fast():")
    print(f"{'='*60}")
    print("  1. C-based task functions (no Python GIL during spawn)")
    print("  2. Pre-allocated C arrays for storage")
    print("  3. Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS around hot path")
    print("  4. Single atomic increment for active_count (batch operation)")
    print("  5. Direct fiber pool allocation without Python overhead")
    print(f"{'='*60}")


if __name__ == '__main__':
    main()
