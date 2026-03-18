# gsyncio Performance Plan (Phased)

## Phase 0: Baseline & Correctness
**Goal:** Make benchmarks trustworthy and eliminate known blockers.

**Work:**
1. Run benchmarks locally and record baseline numbers.
2. Fix `sleep()` timing bug (`gsyncio/async_.py`) and verify with tests.
3. Fix `Future.__await__` bug (`gsyncio/future.py`).
4. Fix WaitGroup hang (C core + Python wrappers).
5. Ensure shutdown completes cleanly.

**Exit criteria:** Benchmarks complete without hangs; tests for sleep and Future pass; baseline numbers recorded in `benchmarks/`.

---

## Phase 1: Hot-Path Python Overhead Reduction
**Goal:** Reduce Python↔C crossings and object churn.

**Work:**
1. Batch task spawn (send N tasks in one C call).
2. Pool task payloads and fiber objects.
3. Avoid per-task closures in loops (prebind callables/args).

**Target:** 3–5× faster task spawn rate.

---

## Phase 2: C Scheduler & Queue Optimizations
**Goal:** Reduce contention and improve work stealing.

**Work:**
1. Lock-free or low-contention MPMC queue.
2. Per-worker fiber pools.
3. Better stealing heuristics to reduce cross-thread hops.

**Target:** 2–3× scheduler throughput.

---

## Phase 3: Stack/Context Improvements
**Goal:** Reduce per-fiber memory + context switch cost.

**Work:**
1. Stack pooling or segmented stacks.
2. Faster context switch path (tighten C hot loop).
3. Inline caching for Python call sites where safe.

**Target:** 1.5–2× context switch rate.

---

## Phase 4: Async I/O Core
**Goal:** Remove asyncio dependency from async path.

**Work:**
1. C-based event loop + I/O primitives.
2. Integrate async/await directly with fiber scheduler.
3. TCP/UDP/file/DNS/SSL groundwork.

**Target:** Real-world async I/O throughput jump.

---

## Phase 5: Validation & Parity Targets
**Goal:** Be honest about parity vs Go in specific workloads.

**Work:**
1. Compare to Go on microbenchmarks.
2. Document where we match or nearly match Go.
3. Publish updated results in `benchmarks/`.

