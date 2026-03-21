"""
Test 2,000 tasks with WaitGroup synchronization
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gsyncio as gs


def worker(wg, worker_id):
    """Worker that signals completion via WaitGroup"""
    try:
        # Simulate some work
        pass
    finally:
        gs.done(wg)


def main():
    """Run 2,000 tasks with WaitGroup"""
    print("Creating WaitGroup...")
    wg = gs.create_wg()
    
    print("Adding 2,000 to WaitGroup...")
    gs.add(wg, 2000)
    
    print("Spawning 2,000 tasks...")
    start = time.time()
    
    for i in range(2000):
        gs.task(worker, wg, i)
    
    spawn_time = time.time() - start
    print(f"All tasks spawned in {spawn_time:.3f}s")
    
    print("Waiting for WaitGroup...")
    gs.wait(wg)
    
    elapsed = time.time() - start
    print(f"\n2,000 tasks completed in {elapsed:.3f}s")
    print(f"Throughput: {2000/elapsed:.0f} tasks/sec")
    print("\n✓ All tasks completed successfully!")


if __name__ == '__main__':
    main()
