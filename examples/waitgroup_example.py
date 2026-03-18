#!/usr/bin/env python3
"""
WaitGroup Example - Synchronization primitive

This example demonstrates using WaitGroup for waiting on
multiple concurrent operations to complete.
"""

import gsyncio as gs
import time
import threading


def worker(wg, worker_id, num_tasks):
    """Worker that processes multiple tasks"""
    try:
        for i in range(num_tasks):
            # Simulate work
            time.sleep(0.01)  # 10ms
            print(f"Worker {worker_id}: completed task {i + 1}/{num_tasks}")
    finally:
        # Signal completion
        gs.done(wg)


def main():
    """Main function demonstrating WaitGroup"""
    print("Starting WaitGroup example...")
    
    # Create wait group
    wg = gs.create_wg()
    
    # Number of workers and tasks per worker
    num_workers = 5
    tasks_per_worker = 4
    
    # Add workers to wait group
    gs.add(wg, num_workers)
    
    # Spawn workers
    print(f"Spawning {num_workers} workers with {tasks_per_worker} tasks each...")
    start = time.time()
    
    for i in range(num_workers):
        gs.task(worker, wg, i, tasks_per_worker)
    
    # Wait for all workers to complete
    print("Waiting for all workers...")
    gs.sync()  # Wait for all tasks first
    
    elapsed = time.time() - start
    print(f"\nAll workers completed in {elapsed:.2f}s")
    
    # Show scheduler stats
    stats = gs.get_scheduler_stats()
    print(f"Scheduler stats: {stats}")


if __name__ == '__main__':
    gs.run(main)
