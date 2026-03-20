"""
Benchmark: gsyncio

This module benchmarks gsyncio's fiber-based concurrency library.

Tests:
- Task creation and spawning (task/sync model)
- Async/await operations
- Concurrent sleep operations
- Batch operations
- Gather operations
"""

import time
import sys
from typing import List

# Import gsyncio
import gsyncio as gs


# ============================================================================
# Task/Sync Model Benchmarks (fire-and-forget parallelism)
# ============================================================================

def simple_sync_task(n: int) -> int:
    """Simple CPU-bound synchronous task."""
    return sum(range(n))


def io_simulated_sync_task(n: int) -> int:
    """Simulated I/O task with computation."""
    result = 0
    for i in range(n):
        result += i * i
    return result


def benchmark_gsyncio_task_spawn(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark task spawning using task/sync model."""
    times = []
    
    for _ in range(iterations):
        # Spawn tasks
        for _ in range(num_tasks):
            gs.task(simple_sync_task, 100)
        
        start = time.perf_counter()
        gs.sync()
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_task_batch(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark batch task spawning."""
    times = []
    
    for _ in range(iterations):
        # Create task list
        tasks = [(simple_sync_task, (100,)) for _ in range(num_tasks)]
        
        start = time.perf_counter()
        gs.task_batch(tasks)
        gs.sync()
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_task_fast(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark fast task spawning."""
    times = []
    
    for _ in range(iterations):
        start = time.perf_counter()
        
        for _ in range(num_tasks):
            gs.task_fast(simple_sync_task, 100)
        
        gs.sync()
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_task_batch_fast(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark fast batch task spawning."""
    times = []
    
    for _ in range(iterations):
        # Create task list
        tasks = [(simple_sync_task, (100,)) for _ in range(num_tasks)]
        
        start = time.perf_counter()
        gs.task_batch_fast(tasks)
        gs.sync()
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


# ============================================================================
# Async/Await Model Benchmarks
# ============================================================================

async def simple_async_task(n: int) -> int:
    """Simple async task."""
    return sum(range(n))


async def sleep_async_task(delay_ms: int) -> int:
    """Async task with sleep."""
    await gs.sleep(delay_ms)
    return delay_ms


async def io_simulated_async_task(n: int) -> int:
    """Simulated I/O async task."""
    result = 0
    for i in range(n):
        result += i * i
    await gs.sleep(1)  # 1ms simulated I/O
    return result


async def chain_async_task(start: int, length: int) -> int:
    """Chain of async awaits."""
    result = start
    for _ in range(length):
        await gs.sleep(0)  # Yield to scheduler
        result += 1
    return result


def benchmark_gsyncio_async_spawn(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark async task creation using run()."""
    times = []
    
    for _ in range(iterations):
        async def run_tasks():
            tasks = [gs.create_task(simple_async_task(100)) for _ in range(num_tasks)]
            return await gs.gather(*tasks)
        
        start = time.perf_counter()
        gs.run(run_tasks())
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_async_sleep(num_tasks: int, delay_ms: int = 10, iterations: int = 10) -> float:
    """Benchmark concurrent async sleep operations."""
    times = []
    
    for _ in range(iterations):
        async def run_tasks():
            tasks = [gs.create_task(sleep_async_task(delay_ms)) for _ in range(num_tasks)]
            return await gs.gather(*tasks)
        
        start = time.perf_counter()
        gs.run(run_tasks())
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_async_gather(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark gather operations."""
    times = []
    
    for _ in range(iterations):
        async def run_gather():
            return await gs.gather(*[io_simulated_async_task(100) for _ in range(num_tasks)])
        
        start = time.perf_counter()
        gs.run(run_gather())
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_async_chain(num_chains: int, chain_length: int, iterations: int = 10) -> float:
    """Benchmark chain of async tasks."""
    times = []
    
    for _ in range(iterations):
        async def run_chains():
            tasks = [chain_async_task(i, chain_length) for i in range(num_chains)]
            return await gs.gather(*tasks)
        
        start = time.perf_counter()
        gs.run(run_chains())
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_gsyncio_async_batch(num_tasks: int, batch_size: int, iterations: int = 10) -> float:
    """Benchmark batch async processing."""
    times = []
    
    async def batch_worker(batch_id: int) -> int:
        results = []
        for i in range(batch_size):
            await gs.sleep(1)
            results.append(batch_id * batch_size + i)
        return sum(results)
    
    for _ in range(iterations):
        async def run_batch():
            batches = num_tasks // batch_size
            return await gs.gather(*[batch_worker(i) for i in range(batches)])
        
        start = time.perf_counter()
        gs.run(run_batch())
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


# ============================================================================
# Mixed Model Benchmarks
# ============================================================================

def benchmark_gsyncio_mixed(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark mixing task/sync and async/await models."""
    times = []
    
    async def async_worker(n: int) -> int:
        await gs.sleep(1)
        return n * 2
    
    def sync_worker(n: int) -> int:
        return n * 3
    
    for _ in range(iterations):
        async def run_mixed():
            # Spawn sync tasks
            for i in range(num_tasks // 2):
                gs.task(sync_worker, i)
            
            # Spawn async tasks
            async_tasks = [gs.create_task(async_worker(i)) for i in range(num_tasks // 2)]
            
            # Wait for all
            gs.sync()
            await gs.gather(*async_tasks)
        
        start = time.perf_counter()
        gs.run(run_mixed())
        end = time.perf_counter()
        
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


# ============================================================================
# Run All Benchmarks
# ============================================================================

def run_all_gsyncio_benchmarks():
    """Run all gsyncio benchmarks and return results."""
    results = {}
    
    print("Running gsyncio benchmarks...")
    print("-" * 50)
    
    # Task/Sync model benchmarks
    print("\n=== Task/Sync Model ===")
    for num_tasks in [100, 1000, 10000, 100000, 1000000, 10000000]:
        key = f"gsyncio_task_spawn_{num_tasks}"
        results[key] = benchmark_gsyncio_task_spawn(num_tasks)
        print(f"Task spawn ({num_tasks} tasks): {results[key]:.4f}s")

    for num_tasks in [100, 1000, 10000, 100000, 1000000, 10000000]:
        key = f"gsyncio_task_batch_{num_tasks}"
        results[key] = benchmark_gsyncio_task_batch(num_tasks)
        print(f"Task batch ({num_tasks} tasks): {results[key]:.4f}s")

    for num_tasks in [100, 1000, 10000, 100000, 1000000, 10000000]:
        key = f"gsyncio_task_fast_{num_tasks}"
        results[key] = benchmark_gsyncio_task_fast(num_tasks)
        print(f"Task fast ({num_tasks} tasks): {results[key]:.4f}s")

    for num_tasks in [100, 1000, 10000, 100000, 1000000, 10000000]:
        key = f"gsyncio_task_batch_fast_{num_tasks}"
        results[key] = benchmark_gsyncio_task_batch_fast(num_tasks)
        print(f"Task batch fast ({num_tasks} tasks): {results[key]:.4f}s")

    # Async/Await model benchmarks
    print("\n=== Async/Await Model ===")
    for num_tasks in [100, 1000, 5000, 50000, 100000, 1000000]:
        key = f"gsyncio_async_spawn_{num_tasks}"
        results[key] = benchmark_gsyncio_async_spawn(num_tasks)
        print(f"Async spawn ({num_tasks} tasks): {results[key]:.4f}s")

    for num_tasks in [100, 1000, 10000, 100000, 1000000]:
        key = f"gsyncio_async_sleep_{num_tasks}"
        results[key] = benchmark_gsyncio_async_sleep(num_tasks)
        print(f"Async sleep ({num_tasks} tasks): {results[key]:.4f}s")

    for num_tasks in [100, 1000, 5000, 50000, 100000]:
        key = f"gsyncio_async_gather_{num_tasks}"
        results[key] = benchmark_gsyncio_async_gather(num_tasks)
        print(f"Async gather ({num_tasks} tasks): {results[key]:.4f}s")
    
    for num_chains in [10, 100, 1000]:
        key = f"gsyncio_async_chain_{num_chains}"
        results[key] = benchmark_gsyncio_async_chain(num_chains, 10)
        print(f"Async chain ({num_chains} chains): {results[key]:.4f}s")
    
    for num_tasks in [100, 1000]:
        key = f"gsyncio_async_batch_{num_tasks}"
        results[key] = benchmark_gsyncio_async_batch(num_tasks, 10)
        print(f"Async batch ({num_tasks} tasks): {results[key]:.4f}s")
    
    # Mixed model benchmarks
    print("\n=== Mixed Model ===")
    for num_tasks in [100, 1000, 5000]:
        key = f"gsyncio_mixed_{num_tasks}"
        results[key] = benchmark_gsyncio_mixed(num_tasks)
        print(f"Mixed ({num_tasks} tasks): {results[key]:.4f}s")
    
    print("-" * 50)
    return results


if __name__ == "__main__":
    run_all_gsyncio_benchmarks()
