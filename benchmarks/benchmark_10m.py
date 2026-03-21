import gsyncio as gs
import time
import sys
import os

def worker(n):
    # Some computation to make the task non-trivial
    # Increased workload to avoid extreme pool contention
    result = 0
    for i in range(1000):
        result += i
    return result

def get_mem_usage():
    """Returns current RSS memory usage in kB."""
    try:
        with open("/proc/self/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except:
        return 0
    return 0

def get_map_count():
    """Returns the number of memory mappings."""
    try:
        with open("/proc/self/maps", "r") as f:
            return sum(1 for _ in f)
    except:
        return 0

def run_benchmark(count):
    print(f"Spawning {count:,} tasks...")
    start = time.time()
    
    mem_before = get_mem_usage()
    maps_before = get_map_count()
    print(f"Memory before: {mem_before} kB, Maps before: {maps_before}")
    
    # Use smaller batches for stability
    batch_size = 50_000
    sync_every = 200_000
    for i in range(0, count, batch_size):
        current_batch = min(batch_size, count - i)
        for _ in range(current_batch):
            gs.task(worker, 0)
        
        total_spawned = i + current_batch
        if total_spawned % sync_every == 0:
            print(f"Spawned {total_spawned:,}... Syncing batch... Maps: {get_map_count()}", flush=True)
            gs.sync()
            print(f"Batch {total_spawned:,} completed. Maps: {get_map_count()}", flush=True)
        elif total_spawned % 50_000 == 0:
            print(f"Spawned {total_spawned:,}... Maps: {get_map_count()}", flush=True)
        
    print(f"Spawning completed in {time.time() - start:.2f}s. Final sync...", flush=True)
    gs.sync()
    
    total_time = time.time() - start
    mem_after = get_mem_usage()
    maps_after = get_map_count()
    
    print(f"Total time for {count:,} tasks: {total_time:.4f}s")
    print(f"Throughput: {count/total_time:.2f} tasks/sec")
    print(f"Memory after: {mem_after} kB (Increase: {mem_after - mem_before} kB)")
    print(f"Maps after: {maps_after} (Increase: {maps_after - maps_before})")

if __name__ == "__main__":
    # Check max_map_count
    try:
        with open("/proc/sys/vm/max_map_count", "r") as f:
            max_maps = int(f.read().strip())
            if max_maps < 2000000:
                print(f"WARNING: vm.max_map_count is {max_maps}, which is too low for 10M fibers.")
                print("Suggest run: sudo sysctl -w vm.max_map_count=20000000")
    except:
        pass

    count = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
    print(f"Starting benchmark for {count} tasks in HYBRID mode...")
    try:
        gs.run(run_benchmark, count, mapping='hybrid')
        print("Benchmark completed successfully!")
    except Exception as e:
        import traceback
        traceback.print_exc()
        sys.exit(1)
