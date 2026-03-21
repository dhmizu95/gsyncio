"""
Test 50,000 concurrent tasks with gsyncio
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gsyncio as gs


def main():
    """Run 50,000 tasks"""
    counter = [0]
    
    def worker():
        counter[0] += 1
    
    print("Initializing scheduler...")
    gs.init_scheduler(num_workers=4, max_fibers=100000)
    
    print("Spawning 50,000 tasks...")
    start = time.time()
    
    for i in range(50000):
        gs.task(worker)
    
    spawn_time = time.time() - start
    print(f"All tasks spawned in {spawn_time:.3f}s")
    
    print("Waiting for all tasks to complete...")
    gs.sync()
    
    elapsed = time.time() - start
    print(f"\n50,000 tasks completed in {elapsed:.3f}s")
    print(f"Throughput: {50000/elapsed:.0f} tasks/sec")
    print(f"Counter value: {counter[0]}")
    
    assert counter[0] == 50000, f"Expected 50000, got {counter[0]}"
    print("\n✓ All tasks completed successfully!")
    
    gs.shutdown_scheduler()


if __name__ == '__main__':
    main()
