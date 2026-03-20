import gsyncio as gs
import time
import pytest

def test_fiber_sleep_actually_waits():
    """Regression test for Bug 1: ensure fiber sleep actually waits"""
    start = time.time()
    
    def worker():
        # Sleep for 100ms
        gs.sleep_ms(100)
    
    gs.task(worker)
    gs.sync()
    
    elapsed = time.time() - start
    print(f"Elapsed: {elapsed:.4f}s")
    assert elapsed >= 0.09  # Allow some margin
    assert elapsed < 0.5   # Should not take way too long

def test_multiple_sleepers():
    """Ensure multiple fibers can sleep concurrently"""
    start = time.time()
    
    def worker(id):
        gs.sleep_ms(100)
    
    for i in range(10):
        gs.task(worker, i)
    
    gs.sync()
    
    elapsed = time.time() - start
    print(f"Total elapsed for 10 sleepers: {elapsed:.4f}s")
    # With 10 workers, they should all finish in ~100ms
    assert elapsed < 0.2
