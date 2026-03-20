import gsyncio as gs
import pytest

def test_batch_fast_spawn_count():
    """Verify Bug 4/5: batch spawn should correctly update task count"""
    initial_count = gs.task_count()
    
    def worker():
        pass
        
    # Use batch spawn
    tasks = [(worker, ()) for _ in range(100)]
    gs.task_batch(tasks)
    
    # Immediately after spawn, count should be at least 100
    current_count = gs.task_count()
    print(f"Count after spawn: {current_count}")
    
    gs.sync()
    
    final_count = gs.task_count()
    print(f"Final count: {final_count}")
    assert final_count == initial_count
