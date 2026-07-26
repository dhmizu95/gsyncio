#!/usr/bin/env python3
"""
Cross-runtime 1M-task comparison: gsyncio (task + async), asyncio, Go.

Each (runtime, workload, n) combination runs in its own process so that
peak RSS is attributable and no runtime warms up another's allocator.

Usage:
    python3 compare_1m.py --run gs_spawn --workload noop -n 1000000
    python3 compare_1m.py --all            # driver: forks one child per cell
"""
import argparse
import gc
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ── measurement helpers ───────────────────────────────────────────────────────

def peak_rss_mb() -> float:
    """Peak resident set size in MiB (Linux VmHWM, else getrusage)."""
    try:
        with open("/proc/self/status") as fh:
            for line in fh:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    import resource
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


# ── workloads ─────────────────────────────────────────────────────────────────
# noop : pure scheduling overhead, zero user work.
# work : ~5 us of pure-Python arithmetic, enough that a runtime which can
#        actually use >1 core would show it.

WORK_N = 2000


def w_noop():
    pass


def w_work():
    s = 0
    for i in range(WORK_N):
        s += i
    return s


async def c_noop():
    pass


async def c_work():
    s = 0
    for i in range(WORK_N):
        s += i
    return s


SYNC_FN = {"noop": w_noop, "work": w_work}
CORO_FN = {"noop": c_noop, "work": c_work}


# ── serial baseline (the "1 core, no runtime" reference point) ────────────────

def run_serial(n, workload):
    fn = SYNC_FN[workload]
    t0 = time.perf_counter()
    for _ in range(n):
        fn()
    t1 = time.perf_counter()
    return {"create_s": 0.0, "total_s": t1 - t0}


# ── asyncio ───────────────────────────────────────────────────────────────────

def run_asyncio_gather(n, workload):
    import asyncio
    fn = CORO_FN[workload]

    async def main():
        t0 = time.perf_counter()
        coros = [fn() for _ in range(n)]
        t1 = time.perf_counter()
        await asyncio.gather(*coros)
        t2 = time.perf_counter()
        return {"create_s": t1 - t0, "total_s": t2 - t0}

    return asyncio.run(main())


def run_asyncio_taskgroup(n, workload):
    import asyncio
    fn = CORO_FN[workload]

    async def main():
        t0 = time.perf_counter()
        async with asyncio.TaskGroup() as tg:
            for _ in range(n):
                tg.create_task(fn())
            t1 = time.perf_counter()
        t2 = time.perf_counter()
        return {"create_s": t1 - t0, "total_s": t2 - t0}

    return asyncio.run(main())


# ── gsyncio: task/sync model ──────────────────────────────────────────────────

def run_gs_spawn(n, workload):
    """Batched spawn: gsyncio's documented fast path (chunks tasks per fiber)."""
    import gsyncio as gs
    gs.init_scheduler(num_workers=0)
    gs.sync()
    fn = SYNC_FN[workload]

    tasks = [(fn, ()) for _ in range(n)]
    gc.collect()
    t0 = time.perf_counter()
    gs.spawn(tasks)
    t1 = time.perf_counter()
    gs.sync()
    t2 = time.perf_counter()
    return {"create_s": t1 - t0, "total_s": t2 - t0, "workers": gs.num_workers()}


def run_gs_spawn_stream(n, workload):
    """Generator-streamed spawn: never materializes the batch (memory path)."""
    import gsyncio as gs
    gs.init_scheduler(num_workers=0)
    gs.sync()
    fn = SYNC_FN[workload]

    gc.collect()
    t0 = time.perf_counter()
    gs.spawn((fn, ()) for _ in range(n))
    t1 = time.perf_counter()
    gs.sync()
    t2 = time.perf_counter()
    return {"create_s": t1 - t0, "total_s": t2 - t0, "workers": gs.num_workers()}


def run_gs_task(n, workload):
    """One real fiber per task - the apples-to-apples goroutine equivalent."""
    import gsyncio as gs
    gs.init_scheduler(num_workers=0)
    gs.sync()
    fn = SYNC_FN[workload]

    gc.collect()
    t0 = time.perf_counter()
    for _ in range(n):
        gs.task(fn)
    t1 = time.perf_counter()
    gs.sync()
    t2 = time.perf_counter()
    return {"create_s": t1 - t0, "total_s": t2 - t0, "workers": gs.num_workers()}


# ── gsyncio: async/await model ────────────────────────────────────────────────

def run_gs_async_gather(n, workload):
    import gsyncio as gs
    gs.init_scheduler(num_workers=0)
    gs.sync()
    fn = CORO_FN[workload]
    box = {}

    async def main():
        t0 = time.perf_counter()
        coros = [fn() for _ in range(n)]
        t1 = time.perf_counter()
        await gs.gather(*coros)
        t2 = time.perf_counter()
        box.update(create_s=t1 - t0, total_s=t2 - t0)

    gc.collect()
    gs.run(main())
    box["workers"] = gs.num_workers()
    return box


def run_gs_async_create_task(n, workload):
    """create_task() per coroutine - no batching, one fiber each."""
    import gsyncio as gs
    gs.init_scheduler(num_workers=0)
    gs.sync()
    fn = CORO_FN[workload]
    box = {}

    async def main():
        t0 = time.perf_counter()
        futs = [gs.create_task(fn()) for _ in range(n)]
        t1 = time.perf_counter()
        for f in futs:
            await f
        t2 = time.perf_counter()
        box.update(create_s=t1 - t0, total_s=t2 - t0)

    gc.collect()
    gs.run(main())
    box["workers"] = gs.num_workers()
    return box


RUNNERS = {
    "serial": run_serial,
    "asyncio_gather": run_asyncio_gather,
    "asyncio_taskgroup": run_asyncio_taskgroup,
    "gs_spawn": run_gs_spawn,
    "gs_spawn_stream": run_gs_spawn_stream,
    "gs_task": run_gs_task,
    "gs_async_gather": run_gs_async_gather,
    "gs_async_create_task": run_gs_async_create_task,
}


# ── child mode: run exactly one cell, emit JSON on stdout ─────────────────────

def run_one(name, n, workload):
    res = RUNNERS[name](n, workload)
    res.update(runtime=name, n=n, workload=workload,
               rss_mb=peak_rss_mb(),
               rate=n / res["total_s"] if res["total_s"] > 0 else float("inf"))
    return res


# ── driver mode: one subprocess per cell ──────────────────────────────────────

DEFAULT_MATRIX = [
    "serial",
    "asyncio_gather", "asyncio_taskgroup",
    "gs_spawn", "gs_spawn_stream", "gs_task",
    "gs_async_gather", "gs_async_create_task",
]


def drive(runtimes, counts, workloads, timeout):
    here = os.path.abspath(__file__)
    rows = []
    for workload in workloads:
        for n in counts:
            for rt in runtimes:
                cmd = [sys.executable, here, "--run", rt,
                       "--workload", workload, "-n", str(n)]
                sys.stderr.write(f"  {rt:<22s} {workload:<5s} n={n:<9d} ... ")
                sys.stderr.flush()
                t0 = time.perf_counter()
                try:
                    out = subprocess.run(cmd, capture_output=True, text=True,
                                         timeout=timeout)
                except subprocess.TimeoutExpired:
                    sys.stderr.write(f"TIMEOUT (>{timeout}s)\n")
                    rows.append({"runtime": rt, "n": n, "workload": workload,
                                 "error": f"timeout>{timeout}s"})
                    continue
                wall = time.perf_counter() - t0
                line = out.stdout.strip().splitlines()
                payload = None
                for ln in reversed(line):
                    if ln.startswith("{"):
                        payload = json.loads(ln)
                        break
                if payload is None:
                    tail = (out.stderr or out.stdout).strip().splitlines()[-3:]
                    sys.stderr.write(f"FAILED: {' | '.join(tail)}\n")
                    rows.append({"runtime": rt, "n": n, "workload": workload,
                                 "error": " | ".join(tail)})
                    continue
                payload["proc_wall_s"] = wall
                rows.append(payload)
                sys.stderr.write(
                    f"{payload['total_s']*1000:9.1f} ms  "
                    f"{payload['rate']/1e6:6.3f}M/s  "
                    f"{payload['rss_mb']:7.1f} MB\n")
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", choices=sorted(RUNNERS))
    ap.add_argument("--workload", default="noop", choices=["noop", "work"])
    ap.add_argument("-n", type=int, default=1_000_000)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--runtimes", default=",".join(DEFAULT_MATRIX))
    ap.add_argument("--counts", default="1000000")
    ap.add_argument("--workloads", default="noop")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if args.run:
        print(json.dumps(run_one(args.run, args.n, args.workload)))
        return

    if args.all:
        rows = drive(args.runtimes.split(","),
                     [int(c) for c in args.counts.split(",")],
                     args.workloads.split(","),
                     args.timeout)
        blob = json.dumps(rows, indent=2)
        if args.out:
            with open(args.out, "w") as fh:
                fh.write(blob)
        else:
            print(blob)
        return

    ap.print_help()


if __name__ == "__main__":
    main()
