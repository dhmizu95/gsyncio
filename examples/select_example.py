#!/usr/bin/env python3
"""
Select Example - Multiplexing on multiple operations

This example demonstrates select-like behavior for handling
multiple types of requests concurrently.
"""

import gsyncio as gs
import time


def handler(name, queue, results):
    """Generic handler that processes items from a queue"""
    count = 0
    while count < 5:  # Process 5 items then exit
        if queue:
            item = queue.pop(0)
            if item:
                result = f"{name}: processed '{item}'"
                results.append(result)
                print(f"  {result}")
                count += 1
        else:
            time.sleep(0.001)
    print(f"  {name} handler done")


def main():
    """Main function demonstrating concurrent request handling"""
    print("Starting select example...")
    
    # Create queues for different request types
    auth_queue = [f"user{i}" for i in range(5)]
    data_queue = [f"query{i}" for i in range(5)]
    results = []
    
    # Spawn handlers for each queue
    print("Starting handlers...")
    gs.task(handler, "Auth", auth_queue, results)
    gs.task(handler, "Data", data_queue, results)
    
    # Wait for all handlers to complete
    gs.sync()
    
    print(f"\nHandled {len(results)} requests")
    
    # Show results
    print("\nAll results:")
    for r in results:
        print(f"  {r}")
    
    # Show scheduler stats
    stats = gs.get_scheduler_stats()
    print(f"\nScheduler stats: {stats}")


if __name__ == '__main__':
    gs.run(main)
