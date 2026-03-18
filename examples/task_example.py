#!/usr/bin/env python3
"""
Task/Sync Example - Fire-and-forget parallelism

This example demonstrates the task/sync model for spawning
concurrent tasks that run in parallel.
"""

import gsyncio as gs


def worker(n):
    """Worker function that does some computation"""
    result = sum(range(n))
    print(f"Worker {n}: sum = {result}")


def main():
    """Main function demonstrating task/sync"""
    print("Spawning 10 workers...")
    
    # Spawn multiple tasks
    for i in range(10):
        gs.task(worker, 1000 * (i + 1))
    
    print(f"Active tasks: {gs.task_count()}")
    
    # Wait for all tasks to complete
    gs.sync()
    
    print("All workers completed!")
    
    # Show scheduler stats
    stats = gs.get_scheduler_stats()
    print(f"Scheduler stats: {stats}")


if __name__ == '__main__':
    gs.run(main)
