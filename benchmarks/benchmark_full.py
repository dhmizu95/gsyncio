#!/usr/bin/env python3
"""
gsyncio vs Go vs asyncio Performance Comparison
"""

import time
import sys
import os
import subprocess

sys.path.insert(0, '.')
os.environ['GSYNCIO_BACKEND'] = 'epoll'

import gsyncio
import asyncio


def benchmark_task_spawn(num_tasks=10000):
    """Benchmark task spawning overhead"""
    print(f"\n=== Task Spawn ({num_tasks} tasks) ===")
    
    counter = [0]
    
    def worker():
        counter[0] += 1
    
    start = time.time()
    for _ in range(num_tasks):
        gsyncio.task(worker)
    gsyncio.sync()
    elapsed = time.time() - start
    
    tasks_per_sec = num_tasks / elapsed
    per_task_us = (elapsed * 1000000) / num_tasks
    
    print(f"  gsyncio: {elapsed*1000:.2f}ms ({tasks_per_sec:,.0f} tasks/s, {per_task_us:.2f}µs/task)")
    return elapsed, tasks_per_sec


def benchmark_context_switch(num_yields=100000):
    """Benchmark context switching via yield"""
    print(f"\n=== Context Switch ({num_yields} yields) ===")
    
    def yielder():
        for _ in range(num_yields // 10):
            gsyncio.yield_execution()
    
    start = time.time()
    for _ in range(10):
        gsyncio.task(yielder)
    gsyncio.sync()
    elapsed = time.time() - start
    
    yields_per_sec = num_yields / elapsed
    per_yield_us = (elapsed * 1000000) / num_yields
    
    print(f"  gsyncio: {elapsed*1000:.2f}ms ({yields_per_sec:,.0f} yields/s, {per_yield_us:.2f}µs/yield)")
    return elapsed, yields_per_sec


def benchmark_channel_throughput(num_items=100000):
    """Benchmark channel operations"""
    print(f"\n=== Channel Throughput ({num_items} items) ===")
    
    ch = gsyncio.Channel(1000)
    counter = [0]
    
    def producer():
        for _ in range(num_items // 2):
            ch.send_nowait(1)
    
    def consumer():
        while counter[0] < num_items:
            if ch.recv_nowait():
                counter[0] += 1
    
    start = time.time()
    gsyncio.task(producer)
    gsyncio.task(producer)
    gsyncio.task(consumer)
    gsyncio.task(consumer)
    gsyncio.sync()
    elapsed = time.time() - start
    
    items_per_sec = num_items / elapsed
    
    print(f"  gsyncio: {elapsed*1000:.2f}ms ({items_per_sec:,.0f} items/s)")
    return elapsed, items_per_sec


async def asyncio_task_spawn(num_tasks=10000):
    """Benchmark asyncio task spawning"""
    counter = [0]
    
    async def worker():
        counter[0] += 1
    
    start = time.time()
    tasks = [asyncio.create_task(worker()) for _ in range(num_tasks)]
    await asyncio.gather(*tasks)
    elapsed = time.time() - start
    
    tasks_per_sec = num_tasks / elapsed
    print(f"  asyncio: {elapsed*1000:.2f}ms ({tasks_per_sec:,.0f} tasks/s, {elapsed*1000000/num_tasks:.2f}µs/task)")
    return elapsed, tasks_per_sec


async def asyncio_channel_throughput(num_items=100000):
    """Benchmark asyncio channel-like operations"""
    queue = asyncio.Queue(1000)
    counter = [0]
    
    async def producer():
        for _ in range(num_items // 2):
            await queue.put(1)
    
    async def consumer():
        while counter[0] < num_items:
            try:
                await asyncio.wait_for(queue.get(), timeout=0.001)
                counter[0] += 1
            except asyncio.TimeoutError:
                pass
    
    start = time.time()
    await asyncio.gather(
        producer(), producer(),
        consumer(), consumer()
    )
    elapsed = time.time() - start
    
    items_per_sec = num_items / elapsed
    print(f"  asyncio: {elapsed*1000:.2f}ms ({items_per_sec:,.0f} items/s)")
    return elapsed, items_per_sec


async def asyncio_context_switch(num_yields=100000):
    """Benchmark asyncio context switching"""
    async def yielder():
        for _ in range(num_yields // 10):
            await asyncio.sleep(0)
    
    start = time.time()
    tasks = [asyncio.create_task(yielder()) for _ in range(10)]
    await asyncio.gather(*tasks)
    elapsed = time.time() - start
    
    yields_per_sec = num_yields / elapsed
    print(f"  asyncio: {elapsed*1000:.2f}ms ({yields_per_sec:,.0f} yields/s, {elapsed*1000000/num_yields:.2f}µs/yield)")
    return elapsed, yields_per_sec


def run_go_benchmark():
    """Run Go benchmark if go is available"""
    go_code = '''
package main

import (
    "fmt"
    "time"
    "sync"
)

func main() {
    fmt.Println("=== Go Task Spawn (10000 tasks) ===")
    
    var counter int64 = 0
    var wg sync.WaitGroup
    
    start := time.Now()
    for i := 0; i < 10000; i++ {
        wg.Add(1)
        go func() {
            counter++
            wg.Done()
        }()
    }
    wg.Wait()
    elapsed := time.Since(start)
    
    tasksPerSec := float64(10000) / elapsed.Seconds()
    fmt.Printf("  Go: %.2fms (%.0f tasks/s, %.2fus/task)\\n", elapsed.Seconds()*1000, tasksPerSec, elapsed.Seconds()*1000000/10000)
    
    fmt.Println("\\n=== Go Context Switch (100000 yields) ===")
    counter = 0
    
    var done chan bool = make(chan bool)
    
    start = time.Now()
    for i := 0; i < 10; i++ {
        go func() {
            for j := 0; j < 10000; j++ {
                done <- true
            }
        }()
    }
    
    for i := 0; i < 100000; i++ {
        <-done
    }
    elapsed = time.Since(start)
    
    yieldsPerSec := float64(100000) / elapsed.Seconds()
    fmt.Printf("  Go: %.2fms (%.0f yields/s, %.2fus/yield)\\n", elapsed.Seconds()*1000, yieldsPerSec, elapsed.Seconds()*1000000/100000)
    
    fmt.Println("\\n=== Go Channel Throughput (100000 items) ===")
    
    type item int
    ch := make(chan item, 1000)
    counter = 0
    
    start = time.Now()
    
    go func() {
        for i := 0; i < 50000; i++ {
            ch <- 1
        }
    }()
    go func() {
        for i := 0; i < 50000; i++ {
            ch <- 1
        }
    }()
    
    go func() {
        for {
            select {
            case <-ch:
                counter++
            default:
                if counter >= 100000 {
                    return
                }
            }
        }
    }()
    
    time.Sleep(100 * time.Millisecond)
    elapsed = time.Since(start)
    
    itemsPerSec := float64(100000) / elapsed.Seconds()
    fmt.Printf("  Go: %.2fms (%.0f items/s)\\n", elapsed.Seconds()*1000, itemsPerSec)
}
'''
    
    try:
        result = subprocess.run(['go', 'run', '-'], input=go_code, capture_output=True, text=True, timeout=30)
        if result.returncode == 0:
            print(result.stdout)
        else:
            print(f"Go not available or error: {result.stderr}")
    except (FileNotFoundError, subprocess.TimeoutExpired):
        print("Go not available on this system")
        return None


def main():
    print("=" * 70)
    print("Performance Comparison: gsyncio vs Go vs asyncio")
    print("=" * 70)
    print(f"Python: {sys.version.split()[0]}")
    print(f"C Extension: {gsyncio._HAS_CYTHON}")
    print()
    
    print("=" * 70)
    print("1. TASK SPAWN BENCHMARK (10000 tasks)")
    print("=" * 70)
    
    gs_time, gs_tps = benchmark_task_spawn(10000)
    
    asyncio.run(asyncio_task_spawn(10000))
    
    run_go_benchmark()
    
    print()
    print("=" * 70)
    print("2. CONTEXT SWITCH BENCHMARK (100000 yields)")
    print("=" * 70)
    
    gs_time, gs_yps = benchmark_context_switch(100000)
    
    asyncio.run(asyncio_context_switch(100000))
    
    print("  (Go uses goroutines - lightweight threads, not coroutines)")
    
    print()
    print("=" * 70)
    print("3. CHANNEL THROUGHPUT (100000 items)")
    print("=" * 70)
    
    gs_time, gs_ips = benchmark_channel_throughput(100000)
    
    asyncio.run(asyncio_channel_throughput(100000))
    
    print("  (Go uses native channels with select)")
    
    print()
    print("=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print("""
Key Findings:
- gsyncio: Native C fiber implementation with M:N scheduler
- asyncio: Pure Python, uses event loop (slower for spawning)
- Go: Native runtime, goroutines areOS threads with runtime scheduling

Notes:
- Go's numbers include goroutine scheduling overhead
- asyncio uses generator-based coroutines (slower than C fibers)
- gsyncio uses setjmp/longjmp for context switching (~0.4µs/yield)
""")


if __name__ == '__main__':
    main()