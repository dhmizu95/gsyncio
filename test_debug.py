#!/usr/bin/env python3
"""
Debug test to find crash location
"""

import gsyncio as gs
import sys

print("Step 1: Import successful", flush=True)

try:
    print("Step 2: Initializing scheduler...", flush=True)
    gs.init_scheduler(num_workers=2)
    print("Step 3: Scheduler initialized", flush=True)
except Exception as e:
    print(f"Scheduler init failed: {e}", flush=True)
    import traceback
    traceback.print_exc()
    sys.exit(1)

def worker(n):
    print(f"Worker {n} running", flush=True)

try:
    print("Step 4: Spawning task 0...", flush=True)
    gs.task(worker, 0)
    print("Step 5: Task 0 spawned", flush=True)
except Exception as e:
    print(f"Task spawn failed: {e}", flush=True)
    import traceback
    traceback.print_exc()
    sys.exit(1)

try:
    print("Step 6: Syncing...", flush=True)
    gs.sync()
    print("Step 7: Sync complete!", flush=True)
except Exception as e:
    print(f"Sync failed: {e}", flush=True)
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("Done!", flush=True)
