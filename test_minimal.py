#!/usr/bin/env python3
"""
Minimal test to reproduce segfault
"""

import gsyncio as gs

def worker(n):
    print(f"Worker {n} running")

print("Spawning tasks...")
for i in range(5):
    gs.task(worker, i)

print("Syncing...")
gs.sync()
print("Done!")
