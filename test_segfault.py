#!/usr/bin/env python3
"""
Test script to reproduce segfault in gsyncio core
"""

import gsyncio as gs
import time

def test_basic_task():
    """Test basic task spawning"""
    print("Testing basic task spawn...")
    results = []

    def worker(n):
        results.append(n)

    for i in range(10):
        gs.task(worker, i)
    
    gs.sync()
    print(f"Results: {results}")
    print(f"Expected: 10 tasks, got: {len(results)}")
    assert len(results) == 10, f"Expected 10 results, got {len(results)}"
    print("✓ Basic task test passed")

def test_many_tasks():
    """Test spawning many tasks"""
    print("\nTesting many tasks...")
    counter = [0]
    
    def increment():
        counter[0] += 1
    
    for i in range(1000):
        gs.task(increment)
    
    gs.sync()
    print(f"Counter: {counter[0]}, Expected: 1000")
    assert counter[0] == 1000, f"Expected 1000, got {counter[0]}"
    print("✓ Many tasks test passed")

def test_nested_tasks():
    """Test nested task spawning"""
    print("\nTesting nested tasks...")
    results = []
    
    def parent(n):
        def child():
            results.append(n)
        gs.task(child)
    
    for i in range(100):
        gs.task(parent, i)
    
    gs.sync()
    print(f"Results: {len(results)}, Expected: 100")
    assert len(results) == 100, f"Expected 100, got {len(results)}"
    print("✓ Nested tasks test passed")

def test_sleep():
    """Test async sleep"""
    print("\nTesting async sleep...")
    
    async def sleeper():
        await gs.sleep(10)  # 10ms
        return "done"
    
    import asyncio
    result = asyncio.run(sleeper())
    print(f"Sleep result: {result}")
    assert result == "done"
    print("✓ Sleep test passed")

def test_channel():
    """Test channel operations"""
    print("\nTesting channel...")
    
    async def sender(ch):
        for i in range(5):
            await gs.send(ch, i)
        gs.close(ch)
    
    async def receiver(ch):
        results = []
        try:
            while True:
                val = await gs.recv(ch)
                results.append(val)
        except:
            pass
        return results
    
    import asyncio
    ch = gs.chan(10)
    asyncio.run(asyncio.gather(sender(ch), receiver(ch)))
    print("✓ Channel test passed")

def test_stress():
    """Stress test with many concurrent operations"""
    print("\nTesting stress...")
    counter = [0]
    
    def worker():
        for _ in range(100):
            counter[0] += 1
    
    for _ in range(100):
        gs.task(worker)
    
    gs.sync()
    print(f"Counter: {counter[0]}, Expected: 10000")
    assert counter[0] == 10000, f"Expected 10000, got {counter[0]}"
    print("✓ Stress test passed")

if __name__ == '__main__':
    print("=" * 60)
    print("gsyncio segfault reproduction tests")
    print("=" * 60)
    
    try:
        test_basic_task()
        test_many_tasks()
        test_nested_tasks()
        test_sleep()
        test_channel()
        test_stress()
        
        print("\n" + "=" * 60)
        print("All tests passed!")
        print("=" * 60)
    except Exception as e:
        print(f"\nTest failed with error: {e}")
        import traceback
        traceback.print_exc()
