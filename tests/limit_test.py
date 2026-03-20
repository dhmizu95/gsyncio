import gsyncio as gs
import time
import os
def get_mem_usage():
    try:
        with open('/proc/self/stat', 'r') as f:
            stats = f.read().split()
            # 24th field is RSS in pages
            rss_pages = int(stats[23])
            page_size = 4096  # Standard for Linux
            return (rss_pages * page_size) / (1024 * 1024)  # MB
    except Exception:
        return 0


def run_performance_test(num_tasks):
    print(f"\n--- Testing with {num_tasks:,} tasks ---")
    
    def worker():
        # Minimal work: just yield once
        gs.yield_execution()
        
    start_mem = get_mem_usage()
    start_time = time.time()
    
    # Spawn tasks in batches of 10k to avoid huge list allocations in Python
    batch_size = 10000
    for i in range(0, num_tasks, batch_size):
        current_batch = min(batch_size, num_tasks - i)
        tasks = [(worker, ()) for _ in range(current_batch)]
        gs.task_batch(tasks)
        if i % 100000 == 0 and i > 0:
            print(f"  Spawned {i:,} tasks...")
            
    spawn_time = time.time() - start_time
    peak_mem = get_mem_usage()
    
    print(f"  Spawning took {spawn_time:.4f}s")
    print(f"  Peak memory usage: {peak_mem:.2f} MB (Delta: {peak_mem - start_mem:.2f} MB)")
    
    print("  Waiting for completion...")
    sync_start = time.time()
    gs.sync()
    sync_time = time.time() - sync_start
    
    total_time = time.time() - start_time
    print(f"  Sync took {sync_time:.4f}s")
    print(f"  Total time: {total_time:.4f}s")
    print(f"  Overall Throughput: {num_tasks / total_time:.2f} tasks/sec")

if __name__ == "__main__":
    # Start with a safe number
    run_performance_test(10000)
    run_performance_test(100000)
    run_performance_test(250000)
    run_performance_test(500000)

