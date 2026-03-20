"""
Benchmark: Python asyncio

This module benchmarks Python's built-in asyncio library for comparison
with gsyncio and Go routines.

Tests:
- Task creation and spawning
- Concurrent sleep operations
- Task gathering
- Batch operations
"""

import asyncio
import time
import sys
from typing import List


async def simple_task(n: int) -> int:
    """Simple CPU-bound task."""
    return sum(range(n))


async def sleep_task(delay_ms: int) -> int:
    """Async sleep task."""
    await asyncio.sleep(delay_ms / 1000.0)
    return delay_ms


async def io_simulated_task(n: int) -> int:
    """Simulated I/O task with small computation."""
    # Simulate some work
    result = 0
    for i in range(n):
        result += i * i
    # Simulate I/O wait
    await asyncio.sleep(0.001)
    return result


def benchmark_asyncio_task_spawn(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark task spawning performance."""
    times = []
    
    for _ in range(iterations):
        start = time.perf_counter()
        
        # Create and schedule tasks
        tasks = [asyncio.create_task(simple_task(100)) for _ in range(num_tasks)]
        
        # Wait for all tasks to complete
        asyncio.get_event_loop().run_until_complete(asyncio.gather(*tasks))
        
        end = time.perf_counter()
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_asyncio_sleep(num_tasks: int, delay_ms: int = 10, iterations: int = 10) -> float:
    """Benchmark concurrent sleep operations."""
    times = []
    
    for _ in range(iterations):
        start = time.perf_counter()
        
        # Create and schedule sleep tasks
        tasks = [asyncio.create_task(sleep_task(delay_ms)) for _ in range(num_tasks)]
        
        # Wait for all tasks to complete
        asyncio.get_event_loop().run_until_complete(asyncio.gather(*tasks))
        
        end = time.perf_counter()
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_asyncio_gather(num_tasks: int, iterations: int = 10) -> float:
    """Benchmark gather operations."""
    times = []
    
    for _ in range(iterations):
        start = time.perf_counter()
        
        # Run gather
        asyncio.get_event_loop().run_until_complete(
            asyncio.gather(*[io_simulated_task(100) for _ in range(num_tasks)])
        )
        
        end = time.perf_counter()
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_asyncio_chain(num_chains: int, chain_length: int, iterations: int = 10) -> float:
    """Benchmark chain of async tasks (simulates await chains)."""
    times = []
    
    async def chain_task(prev_result: int = 0) -> int:
        result = prev_result
        for _ in range(chain_length):
            await asyncio.sleep(0.0001)  # Small delay
            result += 1
        return result
    
    for _ in range(iterations):
        start = time.perf_counter()
        
        # Create chains of tasks
        tasks = [chain_task(i) for i in range(num_chains)]
        asyncio.get_event_loop().run_until_complete(asyncio.gather(*tasks))
        
        end = time.perf_counter()
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def benchmark_asyncio_batch(num_tasks: int, batch_size: int, iterations: int = 10) -> float:
    """Benchmark batch task processing."""
    times = []
    
    async def batch_worker(batch_id: int) -> int:
        results = []
        for i in range(batch_size):
            await asyncio.sleep(0.0001)
            results.append(batch_id * batch_size + i)
        return sum(results)
    
    for _ in range(iterations):
        start = time.perf_counter()
        
        # Process in batches
        batches = num_tasks // batch_size
        all_tasks = [batch_worker(i) for i in range(batches)]
        asyncio.get_event_loop().run_until_complete(asyncio.gather(*all_tasks))
        
        end = time.perf_counter()
        times.append(end - start)
    
    avg_time = sum(times) / len(times)
    return avg_time


def run_all_asyncio_benchmarks():
    """Run all asyncio benchmarks and return results."""
    results = {}
    
    print("Running Python asyncio benchmarks...")
    print("-" * 50)
    
    # Task spawn benchmarks
    for num_tasks in [100, 1000, 10000]:
        key = f"asyncio_task_spawn_{num_tasks}"
        results[key] = benchmark_asyncio_task_spawn(num_tasks)
        print(f"Task spawn ({num_tasks} tasks): {results[key]:.4f}s")
    
    # Sleep benchmarks
    for num_tasks in [100, 1000, 10000]:
        key = f"asyncio_sleep_{num_tasks}"
        results[key] = benchmark_asyncio_sleep(num_tasks)
        print(f"Sleep ({num_tasks} tasks): {results[key]:.4f}s")
    
    # Gather benchmarks
    for num_tasks in [100, 1000, 5000]:
        key = f"asyncio_gather_{num_tasks}"
        results[key] = benchmark_asyncio_gather(num_tasks)
        print(f"Gather ({num_tasks} tasks): {results[key]:.4f}s")
    
    # Chain benchmarks
    for num_chains in [10, 100, 1000]:
        key = f"asyncio_chain_{num_chains}"
        results[key] = benchmark_asyncio_chain(num_chains, 10)
        print(f"Chain ({num_chains} chains): {results[key]:.4f}s")
    
    # Batch benchmarks
    for num_tasks in [100, 1000]:
        key = f"asyncio_batch_{num_tasks}"
        results[key] = benchmark_asyncio_batch(num_tasks, 10)
        print(f"Batch ({num_tasks} tasks): {results[key]:.4f}s")
    
    print("-" * 50)
    return results


if __name__ == "__main__":
    run_all_asyncio_benchmarks()
