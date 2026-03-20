#!/usr/bin/env python3
"""
Debug sync issue - no sleep
"""

import gsyncio as gs
import time
import threading

def worker(n):
    print(f"Worker {n} running")
    time.sleep(0.01)
    print(f"Worker {n} done")

print("Initializing scheduler...")
gs.init_scheduler(num_workers=2)
print("Scheduler initialized")

print("Spawning tasks...")
for i in range(5):
    fid = gs.task(worker, i)
    print(f"Spawned task {i}")

print("Calling sync immediately...")
print("Before sync, active_count =", gs.task_count())
gs.sync()
print("Sync complete!")
print("Final active_count =", gs.task_count())

gs.shutdown_scheduler()
print("Done!")
