#!/usr/bin/env python3
"""
Channel Example - Message passing between fibers

This example demonstrates using channels for communication
between concurrent tasks.
"""

import gsyncio as gs
import time


def worker(worker_id, work_queue, results):
    """Worker that processes work items from a shared queue"""
    while True:
        # Get work item
        if work_queue:
            item = work_queue.pop(0)
            if item is None:  # Shutdown signal
                break
            processed = f"Worker {worker_id}: processed '{item}'"
            results.append(processed)
            print(f"  {processed}")
        else:
            time.sleep(0.001)  # Brief wait


def main():
    """Main function demonstrating concurrent work processing"""
    print("Starting channel-like example...")
    
    # Shared work queue (simulating a channel)
    work_queue = []
    results = []
    
    # Number of workers and tasks
    num_workers = 5
    num_tasks = 20
    
    # Add work items
    for i in range(num_tasks):
        work_queue.append(f"task-{i}")
    
    # Add shutdown signals
    for i in range(num_workers):
        work_queue.append(None)
    
    # Spawn workers
    print(f"Spawning {num_workers} workers with {num_tasks} tasks...")
    for i in range(num_workers):
        gs.task(worker, i, work_queue, results)
    
    # Wait for completion
    gs.sync()
    
    print(f"\nCompleted {len(results)} tasks")
    
    # Show first few results
    print("\nFirst 5 results:")
    for r in results[:5]:
        print(f"  {r}")
    
    # Show scheduler stats
    stats = gs.get_scheduler_stats()
    print(f"\nScheduler stats: {stats}")


if __name__ == '__main__':
    gs.run(main)
