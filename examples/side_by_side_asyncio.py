#!/usr/bin/env python3
"""
side_by_side_asyncio.py - the asyncio half of the matched trio.

Same program as side_by_side.go / side_by_side.py: fan out N workers,
each folds a chunk of the range, push results onto a queue, fan in.

    python3 side_by_side_asyncio.py

Mapping:

    go worker(...)          ->  tg.create_task(worker(...))
    make(chan int64, n)     ->  asyncio.Queue(maxsize=n)
    results <- v            ->  await results.put(v)
    <-results               ->  await results.get()
    wg.Wait()               ->  leaving the `async with TaskGroup` block

Note what asyncio *cannot* express here: there is one thread and one
event loop, so the eight workers do not overlap at all. A coroutine only
yields at an `await`, and this body has none in its hot loop - each
worker runs start to finish before the next one is scheduled. That is
the honest comparison point: asyncio is a concurrency model for I/O
waiting, not a parallelism model for CPU work.
"""

import asyncio
import time

WORKERS = 8
CHUNK_SIZE = 200_000


async def worker(worker_id, results):
    """Fold one chunk and put the checksum on the queue."""
    start = worker_id * CHUNK_SIZE
    total = 0
    for i in range(start, start + CHUNK_SIZE):
        total += i * i
    await results.put(total)


async def main():
    t0 = time.perf_counter()

    results = asyncio.Queue(maxsize=WORKERS)

    # Fan out. TaskGroup waits for every child on block exit, which is
    # what the WaitGroup does on the Go side.
    async with asyncio.TaskGroup() as tg:
        for i in range(WORKERS):
            tg.create_task(worker(i, results))

    # Fan in. Everything has been put by now, so drain without blocking.
    total = 0
    count = 0
    while not results.empty():
        total += results.get_nowait()
        count += 1

    elapsed_ms = (time.perf_counter() - t0) * 1000
    print(f"asyncio: {WORKERS} workers, {count} results, "
          f"total={total}, {elapsed_ms:.1f} ms")


if __name__ == "__main__":
    asyncio.run(main())
