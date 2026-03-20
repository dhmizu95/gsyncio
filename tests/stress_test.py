import gsyncio as gs
import time
import pytest

def test_stress_5000_tasks():
    """Stress test: spawn 5000 tasks and verify they all complete."""
    num_tasks = 5000
    counter = [0]
    
    def worker(val):
        # Do some mock work
        time.sleep(0.00001) 
        counter[0] += 1
        
    start_time = time.time()
    
    # Use batch spawning for efficiency
    tasks = [(worker, (i,)) for i in range(num_tasks)]
    gs.task_batch(tasks)
    
    print(f"Spawned {num_tasks} tasks. Waiting for completion...")
    gs.sync()
    
    end_time = time.time()
    duration = end_time - start_time
    
    print(f"Completed {counter[0]} tasks in {duration:.4f}s")
    print(f"Throughput: {num_tasks / duration:.2f} tasks/sec")
    
    assert counter[0] == num_tasks
    assert gs.task_count() == 0

if __name__ == "__main__":
    test_stress_5000_tasks()
