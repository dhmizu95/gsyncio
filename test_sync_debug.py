#!/usr/bin/env python3
"""
Debug sync issue
"""

import gsyncio as gs
import time
import threading

print("Main thread:", threading.current_thread().name)

def worker(n):
    print(f"Worker {n} running in thread {threading.current_thread().name}")
    time.sleep(0.01)
    print(f"Worker {n} done")

print("Initializing scheduler...")
gs.init_scheduler(num_workers=2)
print("Scheduler initialized")

print("Spawning tasks...")
for i in range(5):
    fid = gs.task(worker, i)
    print(f"Spawned task {i} with fiber_id={fid}")

print("Before sync, active_count =", gs.task_count())
time.sleep(0.1)  # Give tasks time to run
print("After sleep, active_count =", gs.task_count())

print("Calling sync...")
gs.sync()
print("Sync complete!")
print("Final active_count =", gs.task_count())

gs.shutdown_scheduler()
print("Done!")
