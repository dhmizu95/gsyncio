"""
Pytest-compatible benchmarks for gsyncio

This module provides pytest-benchmark compatible benchmarks that can be run
with: pytest benchmarks/pytest_benchmark.py --benchmark-only

These benchmarks compare:
- Python asyncio
- gsyncio (task/sync model)
- gsyncio (async/await model)
"""

import pytest
import asyncio
import time
import gsyncio as gs


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def simple_task():
    """Simple CPU-bound task."""
    def task(n):
        return sum(range(n))
    return task


@pytest.fixture
def async_task():
    """Simple async task."""
    async def task(n):
        return sum(range(n))
    return task


@pytest.fixture
def sleep_async_task():
    """Async task with sleep."""
    async def task(delay_ms):
        await gs.sleep(delay_ms)
        return delay_ms
    return task


# ============================================================================
# Asyncio Benchmarks
# ============================================================================

def test_asyncio_task_spawn_100(benchmark, simple_task):
    """Benchmark asyncio task spawning with 100 tasks."""
    
    def run():
        loop = asyncio.new_event_loop()
        tasks = [asyncio.create_task(simple_task(100)) for _ in range(100)]
        loop.run_until_complete(asyncio.gather(*tasks))
        loop.close()
    
    result = benchmark(run)
    assert result is None


def test_asyncio_task_spawn_1000(benchmark, simple_task):
    """Benchmark asyncio task spawning with 1000 tasks."""
    
    def run():
        loop = asyncio.new_event_loop()
        tasks = [asyncio.create_task(simple_task(100)) for _ in range(1000)]
        loop.run_until_complete(asyncio.gather(*tasks))
        loop.close()
    
    result = benchmark(run)
    assert result is None


def test_asyncio_task_spawn_10000(benchmark, simple_task):
    """Benchmark asyncio task spawning with 10000 tasks."""
    
    def run():
        loop = asyncio.new_event_loop()
        tasks = [asyncio.create_task(simple_task(100)) for _ in range(10000)]
        loop.run_until_complete(asyncio.gather(*tasks))
        loop.close()
    
    result = benchmark(run)
    assert result is None


def test_asyncio_sleep_100(benchmark):
    """Benchmark asyncio concurrent sleep with 100 tasks."""
    
    def run():
        loop = asyncio.new_event_loop()
        tasks = [asyncio.create_task(asyncio.sleep(0.01)) for _ in range(100)]
        loop.run_until_complete(asyncio.gather(*tasks))
        loop.close()
    
    result = benchmark(run)
    assert result is None


def test_asyncio_sleep_1000(benchmark):
    """Benchmark asyncio concurrent sleep with 1000 tasks."""
    
    def run():
        loop = asyncio.new_event_loop()
        tasks = [asyncio.create_task(asyncio.sleep(0.01)) for _ in range(1000)]
        loop.run_until_complete(asyncio.gather(*tasks))
        loop.close()
    
    result = benchmark(run)
    assert result is None


# ============================================================================
# gsyncio Task/Sync Model Benchmarks
# ============================================================================

def test_gsyncio_task_spawn_100(benchmark, simple_task):
    """Benchmark gsyncio task spawning with 100 tasks."""
    
    def run():
        for _ in range(100):
            gs.task(simple_task, 100)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_spawn_1000(benchmark, simple_task):
    """Benchmark gsyncio task spawning with 1000 tasks."""
    
    def run():
        for _ in range(1000):
            gs.task(simple_task, 100)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_spawn_10000(benchmark, simple_task):
    """Benchmark gsyncio task spawning with 10000 tasks."""
    
    def run():
        for _ in range(10000):
            gs.task(simple_task, 100)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_fast_100(benchmark, simple_task):
    """Benchmark gsyncio fast task spawning with 100 tasks."""
    
    def run():
        for _ in range(100):
            gs.task_fast(simple_task, 100)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_fast_1000(benchmark, simple_task):
    """Benchmark gsyncio fast task spawning with 1000 tasks."""
    
    def run():
        for _ in range(1000):
            gs.task_fast(simple_task, 100)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_batch_100(benchmark, simple_task):
    """Benchmark gsyncio batch task spawning with 100 tasks."""
    
    def run():
        tasks = [(simple_task, (100,)) for _ in range(100)]
        gs.task_batch(tasks)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_batch_1000(benchmark, simple_task):
    """Benchmark gsyncio batch task spawning with 1000 tasks."""
    
    def run():
        tasks = [(simple_task, (100,)) for _ in range(1000)]
        gs.task_batch(tasks)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_task_batch_10000(benchmark, simple_task):
    """Benchmark gsyncio batch task spawning with 10000 tasks."""
    
    def run():
        tasks = [(simple_task, (100,)) for _ in range(10000)]
        gs.task_batch(tasks)
        gs.sync()
    
    result = benchmark(run)
    assert result is None


# ============================================================================
# gsyncio Async/Await Model Benchmarks
# ============================================================================

def test_gsyncio_async_spawn_100(benchmark, async_task):
    """Benchmark gsyncio async task spawning with 100 tasks."""
    
    def run():
        async def run_tasks():
            tasks = [gs.create_task(async_task(100)) for _ in range(100)]
            return await gs.gather(*tasks)
        
        gs.run(run_tasks())
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_async_spawn_1000(benchmark, async_task):
    """Benchmark gsyncio async task spawning with 1000 tasks."""
    
    def run():
        async def run_tasks():
            tasks = [gs.create_task(async_task(100)) for _ in range(1000)]
            return await gs.gather(*tasks)
        
        gs.run(run_tasks())
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_async_sleep_100(benchmark, sleep_async_task):
    """Benchmark gsyncio async sleep with 100 tasks."""
    
    def run():
        async def run_tasks():
            tasks = [gs.create_task(sleep_async_task(10)) for _ in range(100)]
            return await gs.gather(*tasks)
        
        gs.run(run_tasks())
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_async_sleep_1000(benchmark, sleep_async_task):
    """Benchmark gsyncio async sleep with 1000 tasks."""
    
    def run():
        async def run_tasks():
            tasks = [gs.create_task(sleep_async_task(10)) for _ in range(1000)]
            return await gs.gather(*tasks)
        
        gs.run(run_tasks())
    
    result = benchmark(run)
    assert result is None


# ============================================================================
# Additional Benchmarks
# ============================================================================

def test_gsyncio_mixed_100(benchmark, simple_task):
    """Benchmark mixing task/sync and async/await models."""
    
    async def async_worker(n):
        await gs.sleep(1)
        return n * 2
    
    def sync_worker(n):
        return n * 3
    
    def run():
        async def run_mixed():
            # Spawn sync tasks
            for i in range(50):
                gs.task(sync_worker, i)
            
            # Spawn async tasks
            async_tasks = [gs.create_task(async_worker(i)) for i in range(50)]
            
            # Wait for all
            gs.sync()
            await gs.gather(*async_tasks)
        
        gs.run(run_mixed())
    
    result = benchmark(run)
    assert result is None


def test_gsyncio_channel_100(benchmark):
    """Benchmark gsyncio channel operations."""
    
    def run():
        ch = gs.chan()
        
        async def sender():
            for i in range(50):
                await ch.send(i)
        
        async def receiver():
            for _ in range(50):
                await ch.recv()
        
        async def main():
            await gs.gather(sender(), receiver())
        
        gs.run(main())
    
    result = benchmark(run)
    assert result is None
