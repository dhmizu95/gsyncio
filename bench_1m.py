#!/usr/bin/env python3
"""
gsyncio 1M-task Performance Benchmark
======================================

Tests task spawning throughput and sync() latency at scale:
  • 1,000 tasks
  • 10,000 tasks
  • 100,000 tasks
  • 1,000,000 tasks

Spawn modes tested:
  • spawn_batch_fast()  – bulk spawn (no fiber-ID capture)

Also benchmarks:
  • Channel throughput (1M send/recv)
  • Atomic counter throughput
  • Scheduler stats overhead
"""

import time
import sys
import os
import gc
import threading

# ── Ensure the project root is on the path ────────────────────────────────────
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gsyncio as gs

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _fmt(n: float, unit: str = "") -> str:
    if n >= 1_000_000:
        return f"{n/1_000_000:.2f}M{unit}"
    if n >= 1_000:
        return f"{n/1_000:.1f}k{unit}"
    return f"{n:.1f}{unit}"


def _rate(count: int, elapsed: float) -> str:
    if elapsed == 0:
        return "inf"
    r = count / elapsed
    return f"{_fmt(r, '/s')} ({elapsed*1000:.1f} ms)"


SEPARATOR = "-" * 70


# ─────────────────────────────────────────────────────────────────────────────
# null task — measures pure scheduler overhead (no work)
# ─────────────────────────────────────────────────────────────────────────────

def _noop():
    pass   # just entry + return overhead


# ─────────────────────────────────────────────────────────────────────────────
# Benchmark: spawn_batch_fast() mode
# ─────────────────────────────────────────────────────────────────────────────

def bench_spawn_batch_fast(n: int) -> dict:
    tasks = [(_noop, ()) for _ in range(n)]
    gc.disable()
    t0 = time.perf_counter()
    gs.spawn_batch_fast(tasks)
    t_spawn = time.perf_counter()
    gs.sync()
    t_sync = time.perf_counter()
    gc.enable()

    return {
        "mode": "spawn_batch_fast",
        "n": n,
        "spawn_s": t_spawn - t0,
        "sync_s": t_sync - t_spawn,
        "total_s": t_sync - t0,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Benchmark: C task system (GIL-free tasks)
# ─────────────────────────────────────────────────────────────────────────────

def bench_c_tasks(n: int) -> dict:
    from gsyncio._gsyncio_core import (
        c_task_spawn_sum_squares, c_task_reset_stats_py, c_task_get_stats_py,
    )
    gc.disable()
    c_task_reset_stats_py()
    t0 = time.perf_counter()
    for i in range(n):
        c_task_spawn_sum_squares(i % 1000)
    t_spawn = time.perf_counter()
    gs.sync()
    t_sync = time.perf_counter()
    stats = c_task_get_stats_py()
    gc.enable()

    return {
        "mode": "c_task (GIL-free)",
        "n": n,
        "spawn_s": t_spawn - t0,
        "sync_s": t_sync - t_spawn,
        "total_s": t_sync - t0,
        "c_task_time_ns": stats.get("c_task_time_ns", 0),
        "spawned": stats.get("c_tasks_spawned", 0),
    }


# ─────────────────────────────────────────────────────────────────────────────
# Benchmark: Atomic counter throughput
# ─────────────────────────────────────────────────────────────────────────────

def bench_atomics(n: int) -> dict:
    gc.disable()
    t0 = time.perf_counter()
    for _ in range(n):
        gs.atomic_inc_task_count()
    for _ in range(n):
        gs.atomic_dec_task_count()
    t1 = time.perf_counter()
    gc.enable()

    return {
        "mode": "atomic inc+dec",
        "n": n * 2,
        "total_s": t1 - t0,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Benchmark: Channel throughput (non-blocking)
# ─────────────────────────────────────────────────────────────────────────────

def bench_channel(n: int) -> dict:
    ch = gs.Channel(n)

    gc.disable()
    t0 = time.perf_counter()
    for i in range(n):
        ch.send_nowait(i)
    t_fill = time.perf_counter()
    for _ in range(n):
        ch.recv_nowait()
    t_drain = time.perf_counter()
    gc.enable()
    ch.close()

    return {
        "mode": "channel nowait",
        "n": n,
        "fill_s": t_fill - t0,
        "drain_s": t_drain - t_fill,
        "total_s": t_drain - t0,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Pretty printers
# ─────────────────────────────────────────────────────────────────────────────

def print_task_result(r: dict):
    n     = r["n"]
    mode  = r["mode"]
    spawn = r["spawn_s"]
    sync_ = r["sync_s"]
    total = r["total_s"]

    spawn_rate = n / spawn if spawn > 0 else float("inf")
    total_rate = n / total if total > 0 else float("inf")

    print(f"  [{mode}]  n={_fmt(n)}")
    print(f"    spawn : {spawn*1000:8.1f} ms   -> {_fmt(spawn_rate,'/s')}")
    print(f"    sync  : {sync_*1000:8.1f} ms")
    print(f"    total : {total*1000:8.1f} ms   -> {_fmt(total_rate,'/s')} end-to-end")
    if "c_task_time_ns" in r and r["c_task_time_ns"] > 0:
        ct = r["c_task_time_ns"]
        sp = max(r.get("spawned", n), 1)
        print(f"    c_ns  : {ct/1e6:8.1f} ms total,  {ct//sp} ns/task")
    print()


def print_atomic_result(r: dict):
    ops  = r["n"]
    tot  = r["total_s"]
    rate = ops / tot if tot > 0 else float("inf")
    print(f"  [{r['mode']}]  ops={_fmt(ops)}")
    print(f"    total : {tot*1000:8.1f} ms   -> {_fmt(rate,'/s')}")
    print()


def print_channel_result(r: dict):
    n    = r["n"]
    tot  = r["total_s"]
    rate = (n * 2) / tot if tot > 0 else float("inf")
    print(f"  [{r['mode']}]  items={_fmt(n)}")
    print(f"    fill  : {r['fill_s']*1000:8.1f} ms")
    print(f"    drain : {r['drain_s']*1000:8.1f} ms")
    print(f"    total : {tot*1000:8.1f} ms   -> {_fmt(rate,'/s')} ops (send+recv)")
    print()


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    print()
    print("=" * 70)
    print("  gsyncio  Performance Benchmark  v0.2  -- 1M tasks")
    print("=" * 70)
    print(f"  Python        : {sys.version.split()[0]}")
    print(f"  HAS_CYTHON    : {gs._HAS_CYTHON}")
    print(f"  HAS_NATIVE_IO : {gs._HAS_NATIVE_IO}")

    gs.init_scheduler(num_workers=0)   # 0 = auto-detect CPUs
    gs.sync()
    print(f"  num_workers   : {gs.num_workers()}")
    print()

    COUNTS = [1_000, 10_000, 100_000, 1_000_000]

    # ── 1. spawn_batch_fast() ────────────────────────────────────────────────
    print(SEPARATOR)
    print("  spawn_batch_fast()  -- bulk, minimal error check")
    print(SEPARATOR)
    for n in COUNTS:
        try:
            r = bench_spawn_batch_fast(n)
            print_task_result(r)
        except Exception as e:
            print(f"  ERROR n={_fmt(n)}: {e}\n")

    # ── 2. C-native tasks (GIL-free) ─────────────────────────────────────────
    if gs._HAS_CYTHON:
        print(SEPARATOR)
        print("  C native tasks  -- GIL-free execution (sum_squares)")
        print(SEPARATOR)
        for n in [1_000, 10_000, 100_000]:
            try:
                r = bench_c_tasks(n)
                print_task_result(r)
            except Exception as e:
                print(f"  ERROR n={_fmt(n)}: {e}\n")

    # ── 3. Atomics ────────────────────────────────────────────────────────────
    print(SEPARATOR)
    print("  Atomic counter  -- inc + dec throughput")
    print(SEPARATOR)
    for n in [100_000, 1_000_000]:
        r = bench_atomics(n)
        print_atomic_result(r)

    # ── 4. Channel throughput ─────────────────────────────────────────────────
    if gs._HAS_CYTHON:
        print(SEPARATOR)
        print("  Channel  -- buffered non-blocking send/recv")
        print(SEPARATOR)
        for n in [10_000, 100_000, 1_000_000]:
            try:
                r = bench_channel(n)
                print_channel_result(r)
            except Exception as e:
                print(f"  ERROR n={_fmt(n)}: {e}\n")

    # ── Stats ─────────────────────────────────────────────────────────────────
    print(SEPARATOR)
    print("  Scheduler Stats")
    print(SEPARATOR)
    stats = gs.get_scheduler_stats()
    for k, v in stats.items():
        print(f"  {k:<35s}: {_fmt(v)}")
    print()

    print("=" * 70)
    print("  Benchmark Complete")
    print("=" * 70)
    print()

    gs.shutdown_scheduler(wait=True)


if __name__ == "__main__":
    main()
