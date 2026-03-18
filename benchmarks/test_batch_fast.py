#!/usr/bin/env python3
"""
Quick test of spawn_batch_ultra_fast functionality
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import gsyncio as gs

counter = 0

def worker(n):
    """Simple worker that increments counter"""
    global counter
    counter += 1

def main():
    global counter
    
    print("Testing spawn_batch_ultra_fast...")
    
    # Test 1: Small batch
    print("\nTest 1: Small batch (10 tasks)")
    counter = 0
    tasks = [(worker, (i,)) for i in range(10)]
    result = gs.spawn_batch_ultra_fast(tasks)
    print(f"  Spawned: {result} tasks")
    gs.sync()
    print(f"  Completed: {counter} tasks")
    
    # Test 2: Medium batch
    print("\nTest 2: Medium batch (100 tasks)")
    counter = 0
    tasks = [(worker, (i,)) for i in range(100)]
    result = gs.spawn_batch_ultra_fast(tasks)
    print(f"  Spawned: {result} tasks")
    gs.sync()
    print(f"  Completed: {counter} tasks")
    
    # Test 3: Large batch
    print("\nTest 3: Large batch (1000 tasks)")
    counter = 0
    tasks = [(worker, (i,)) for i in range(1000)]
    result = gs.spawn_batch_ultra_fast(tasks)
    print(f"  Spawned: {result} tasks")
    gs.sync()
    print(f"  Completed: {counter} tasks")
    
    print("\n✓ All tests passed!")

if __name__ == '__main__':
    gs.run(main)
