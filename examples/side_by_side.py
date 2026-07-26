#!/usr/bin/env python3
"""
side_by_side.py - the gsyncio half of a matched pair.

Same program as side_by_side.go: fan out N workers, each computes a
checksum over a slice of the range, push results down a channel, and
have a collector fold them into a total.

    python3 side_by_side.py

Mapping to Go:

    go worker(...)          ->  gs.task(worker, ...)
    var wg sync.WaitGroup   ->  wg = gs.create_wg()
    wg.Add(1) / wg.Done()   ->  gs.add(wg, 1) / gs.done(wg)
    wg.Wait()               ->  gs.sync()   (waits for every task)
    make(chan int64, n)     ->  gs.create_chan(n)
    results <- v            ->  ch.send_nowait(v)
    <-results               ->  ch.recv_nowait()
    close(results)          ->  gs.close(ch)

Workers must be plain functions passed to gs.task(), not `async def`
coroutines - channel send/recv park the fiber in C, which is the
goroutine model. Coroutines are for the async/await API instead.
"""

import os
import sys
import time

# Import the gsyncio in this repo, not whatever else is on the path.
# Running from examples/ puts this directory on sys.path but not the
# project root, so a stale gsyncio installed elsewhere would win.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import gsyncio as gs

WORKERS = 8
CHUNK_SIZE = 200_000


def worker(worker_id, results):
    """Compute a checksum for one chunk and send it on the channel."""
    start = worker_id * CHUNK_SIZE
    total = 0
    for i in range(start, start + CHUNK_SIZE):
        total += i * i
    results.send_nowait(total)


def main():
    t0 = time.perf_counter()

    results = gs.create_chan(WORKERS)

    # Fan out: one fiber per chunk. gs.task() is fire-and-forget, exactly
    # like `go f()` - it returns immediately with no handle to join.
    for i in range(WORKERS):
        gs.task(worker, i, results)

    # gs.sync() waits for every spawned task, which is what the
    # WaitGroup + closer goroutine does on the Go side.
    gs.sync()
    gs.close(results)

    # Fan in. Everything has already been sent, so drain without blocking.
    total = 0
    count = 0
    while True:
        value = results.recv_nowait()
        if value is None:
            break
        total += value
        count += 1

    elapsed_ms = (time.perf_counter() - t0) * 1000
    print(f"gsyncio: {WORKERS} workers, {count} results, "
          f"total={total}, {elapsed_ms:.1f} ms")


if __name__ == "__main__":
    gs.run(main)
