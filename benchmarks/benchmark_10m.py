import gsyncio as gs
import time
import os

def worker(n):
    # Perform some actual CPU-bound work
    result = 0
    for i in range(100):
        result += i
    return result

def run_benchmark():
    import sys
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 10_000_000
    print(f"Spawning {count:,} tasks...")
    
    def get_mem():
        with open("/proc/self/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return line.split()[1] + " " + line.split()[2]
        return "N/A"

    print(f"Memory before: {get_mem()}")
    start = time.time()
    
    # Use batch spawn for efficiency
    batch_size = 100_000
    for i in range(0, count, batch_size):
        current_batch = min(batch_size, count - i)
        for _ in range(current_batch):
            gs.task(worker, 0)
        if count > batch_size:
            print(f"Spawned {i + current_batch:,}...")
        
    print(f"Spawning completed in {time.time() - start:.2f}s. Waiting for completion...")
    
    gs.sync()
    
    total_time = time.time() - start
    print(f"Total time for {count:,} tasks: {total_time:.4f}s")
    print(f"Throughput: {count / total_time:.2f} tasks/sec")
    print(f"Memory after: {get_mem()}")

if __name__ == "__main__":
    # Check max_map_count
    try:
        with open("/proc/sys/vm/max_map_count", "r") as f:
            max_map = int(f.read().strip())
            if max_map < 10_100_000:
                print(f"WARNING: vm.max_map_count is {max_map}, which is too low for 10M fibers.")
                print("Suggest run: sudo sysctl -w vm.max_map_count=20000000")
    except:
        pass

    gs.run(run_benchmark)
