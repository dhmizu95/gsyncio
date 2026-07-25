# Closing the gap to Go goroutine performance

Benchmark context (n=100k, 12 workers): Go goroutines ran ~14-54x faster than
gsyncio, and ~5-24x faster than asyncio, on both a no-op task and a
`sum_squares(n % 1000)` CPU task. See conversation / bench scripts in
scratchpad for raw numbers.

## Why gsyncio can't fully close the gap

Two structural costs Go's runtime doesn't have to pay at all:

- **No GIL in Go.** Any gsyncio path that touches a Python object
  serializes on the GIL.
- **Compiled native code vs. bytecode interpreter.** Go tasks run as
  machine code; Python callables always go through the interpreter.

These two alone account for most of the gap and are not fixable without
leaving CPython.

## Self-inflicted overhead worth fixing, ranked by expected impact

1. **Mutex-protected fiber pool (`fiber_pool_alloc`/`fiber_pool_free` in
   `csrc/fiber_pool.c`).** Every spawn takes a *global* lock. Go gives
   each P its own local free list and only touches a shared lock when
   that's empty. Shard the fiber pool per-worker (each worker has its
   own free list, refilled from a shared pool only when empty) to
   remove the single biggest contention point under concurrent spawn.
   **Highest value, also the riskiest** - touches the same code where a
   real work-stealing-deque race was just fixed (missing CAS in
   `pop_top`/`steal_bottom`, csrc/scheduler.c). Needs the same
   gdb-verified stress testing before trusting it.

2. **`malloc`/`free` per task.** `c_task_spawn_batch_int`
   (csrc/c_tasks.c) still mallocs a wrapper + an arg box for every
   single task, even in the batched path. Go's stacks come from
   size-classed local caches. A pooled/arena allocator (batch-allocate
   N wrappers as one array, hand out slices) would cut this close to
   zero.

3. **Lock-free free-list instead of `pthread_mutex`.** The fiber pool's
   free list is just a stack (push/pop) - a textbook CAS loop, no mutex
   needed.

4. **Fewer GIL transitions on the Python-callable path.**
   `spawn_batch_fast`'s bottleneck is one `PyGILState_Ensure`/`Release`
   pair *per task* (in `_c_fiber_entry`, gsyncio/_gsyncio_core.pyx).
   Having a worker drain several queued Python callables under a single
   GIL acquisition would amortize that cost across tasks instead of
   paying it every time.

5. **Sharded stats counters.** `g_stats_mutex` (csrc/c_tasks.c) is taken
   on every spawn and every completion. The task-count path already got
   sharded per-worker (`scheduler_sharded_inc_task_count`); the C-task
   stats path didn't.

## Expected payoff

None of this reaches Go's ~1M+/s - that ceiling is set by "interpreted +
GIL," full stop. But #1-#3 together are plausibly a **2-5x** win on the
GIL-free `c_task` path specifically, since that path's overhead is now
almost entirely scheduler bookkeeping, not GIL cost.

## Status

**#1 (sharded fiber pool) - done.**

Implemented in `csrc/fiber_pool.c`/`csrc/fiber_pool.h`: the pool is now
`FIBER_POOL_NUM_SHARDS` (64) independent shards, each with its own
mutex + free list. Shard selection reuses the scheduler's existing
`scheduler_get_current_worker_id()` (same mechanism already used for
the sharded task/completion counters), so a worker frees the fiber it
just ran back into its own shard, and whoever allocates next contends
on a 1-of-64 lock instead of one pool-wide mutex. Growth (when a
shard's free list is empty) uses a single lock-free atomic counter
shared across all shards, so it never contends with any shard's lock
either. No call-site or function-signature changes were needed
(`fiber_pool_alloc(pool)`/`fiber_pool_free(pool, fiber)` kept their
exact signatures) - the sharding is entirely internal.

Result: n=100k, `c_task_spawn_batch_sum_squares` went from ~87.7k/s to
~191.1k/s (~2.2x), individual `c_task_spawn_sum_squares` went from
~73.6k/s to ~174.8k/s (~2.4x). Matches the predicted 2-5x range.

Verified: 33/33 pytest suite, 15x repeated stress runs (the same repro
scripts that caught the earlier work-stealing-deque race and the
ultra_fast double-free) at up to 100k tasks, plus a `gdb`-batch run -
all clean, no crashes, no hangs.

**#2 (arena allocator for wrapper+arg) - done, with one caveat found along the way.**

Implemented in `csrc/c_tasks.c`: `c_task_spawn_batch_int` now allocates
one `c_task_arena_t` (wrappers array + args array + an atomic refcount)
per batch instead of two `malloc`s per task. Each task gets a slice
(`&wrappers[i]`, `&args[i]`) instead of its own allocation. Fibers from
the same batch finish at different times, so the arena is freed only
when the last one completes (`arena_release()` decrements the refcount,
frees on reaching zero). Individually-spawned tasks (`c_task_spawn`/
`_int`/`_int_int`) are unaffected - `wrapper->arena = NULL` for those,
and `c_task_wrapper()` branches on that to free them the old way.

While validating this, a leak-shaped regression from item #1 surfaced:
RSS grew linearly and unboundedly (~15.6 MB per 50k-task batch, no
plateau after 20+ iterations) - present on `spawn_batch_fast` too
(untouched by item #2), proving it was item #1's fault, not the arena.
Root cause: `fiber_pool_alloc()` picked a shard from the *calling*
thread's identity, but the calling thread (Python/main) is almost never
the worker that will actually run the fiber - so allocation and the
later `fiber_pool_free()` (called by the real executing worker) almost
never hit the same shard. One shard kept starving and growing forever
while the other 11 accumulated free fibers nobody ever drew from.

Fixed by passing the fiber's actual target worker ID into
`fiber_pool_alloc(pool, worker_hint)` - computed from the existing
round-robin worker selection in `scheduler_spawn()` and
`task_batch_fast_spawn_nogil()` *before* allocating, instead of after.
Cut growth ~8x (~1.8 MB per 50k-batch remaining). The residual growth
was confirmed (via a temporary debug counter) to be real peak-in-flight
concurrency under this specific stress pattern (spawning 50k tasks
faster than 12 workers can drain them) landing on shards 4-11 only,
not a leak - a growable pool legitimately has to grow to its peak
concurrent usage at least once.

Result: n=100k, `c_task_spawn_batch_sum_squares` went from ~191.1k/s
(after item #1 alone) to ~222.6k/s - a further ~1.16x, ~2.5x
cumulative from the pre-#1 baseline of ~87.7k/s.

Verified: 33/33 pytest suite, the same repro scripts from item #1
(15x clean + a `gdb` pass), plus two RSS-growth checks (before/after
the shard-affinity fix) and a shard-distribution debug trace to confirm
the residual growth's cause.

**Known issue found, not fixed:** one intermittent crash (~1 in ~120
runs) during interpreter shutdown - `Fatal Python error:
PyGILState_Release: auto-releasing thread-state, but no thread-state
for this thread`, happening in `Py_Finalize`/`PyGILState_Release`
after all spawned work had already completed successfully. Could not
reproduce again across 105 further runs (60 individual + 45 combined
sequential). This looks like a pre-existing race between a worker
thread's `with gil:` block in `_c_fiber_entry` (gsyncio/_gsyncio_core.pyx)
and `gs.shutdown_scheduler(wait=True)` / interpreter teardown - i.e. a
worker thread hasn't fully finished acquiring/releasing the GIL by the
time `Py_Finalize` starts tearing down thread states. Not something
introduced by items #1/#2 specifically (it's in the shutdown path, not
the spawn/pool code either item touched), but flagging here since it
surfaced during this work. Needs dedicated investigation - likely a
`shutdown_scheduler(wait=True)` that doesn't actually guarantee every
worker's in-flight GIL acquisition has settled before returning.

**#3 (spinlock instead of mutex) - done, after a detour.**

The plan's original wording ("textbook CAS loop, no mutex needed") was
rejected on reflection: a naive lock-free Treiber stack has the classic
ABA problem (thread A reads `head=X`, gets preempted; thread B pops X,
pops the next node, pushes X back with a *different* `next` - thread
A's CAS then succeeds against stale state and corrupts the list).
Fixing that properly needs tagged/versioned pointers (128-bit CAS) -
too much added risk in a subsystem that had already produced three
serious concurrency bugs this session. Implemented a `pthread_spinlock_t`
per shard instead of `pthread_mutex_t`: still true mutual exclusion (zero
ABA risk), just avoids the mutex's futex/syscall path for these very
short (few-pointer-op) critical sections. `csrc/fiber_pool.c`/`.h`.

**False alarm, corrected:** initial stress testing (100x repro.py, an
ultra_fast repro script) showed 3/100 failures with the spinlock vs. a
remembered ~0.9% mutex baseline, so the spinlock was reverted and
blamed. That comparison turned out to be apples-to-oranges - the 0.9%
figure came from a *mixed* sample across three different repro scripts
during item #2's validation, not from repro.py alone. Re-measuring the
*reverted* mutex build against repro.py alone also gave 3/100 - proving
the spinlock was never the cause. Caught under `gdb` (attempt 54/60):
`Fatal Python error: PyGILState_Release: auto-releasing thread-state,
but no thread-state for this thread` / `Python runtime state:
finalizing`. Actual root cause: `repro.py` spawns 10,000 fibers via
`spawn_batch_ultra_fast` and exits *without calling `gs.sync()`* -
letting the interpreter start finalizing while worker threads are still
mid-flight acquiring/releasing the GIL in `_c_fiber_entry`
(gsyncio/_gsyncio_core.pyx). Adding a trailing `gs.sync()` to the repro
script: **0/100 failures**, both with the mutex and with the spinlock
reinstated. This is a hazard of not synchronizing before process exit
(a known general risk with native-thread Python extensions), not a
gsyncio correctness bug, and not something either lock type causes or
fixes. Worth hardening defensively at some point (e.g. an atexit hook
that force-syncs), but that's separate from #1-#5 and not planned yet.

Result: n=100k, `c_task_spawn_batch_sum_squares` went from ~222.6k/s
(item #2, mutex) to ~248.2k/s (~1.11x further, ~2.8x cumulative from
the pre-#1 baseline of ~87.7k/s).

Verified: 33/33 pytest suite, 100x repro.py both with and without the
trailing `sync()` (0/100 and 3/100 respectively, identical for both
lock types), 15x the item #1/#2 repro scripts (all synced) clean, a
`gdb` pass.

**#4 (batched GIL acquisition) - done.**

Implemented in `gsyncio/_gsyncio_core.pyx`: added `_c_fiber_entry_chunk`,
a fiber entry point that takes a LIST of (func, args) tuples instead of
a single one, acquires the GIL ONCE, and loops through every task in
its chunk before releasing it (still isolating exceptions per-task, so
one raising task doesn't stop the rest of its chunk). `spawn_batch_fast`
now chunks its input (~8 chunks per worker, so `chunk_size = count //
(num_workers * 8)`, minimum 1) and spawns one fiber per chunk instead
of one fiber per task.

This is a change in parallelism granularity, not just a constant-factor
speedup: instead of N independently-scheduled fibers, there are now
~`8 * num_workers` fibers, each running a slice of tasks back-to-back
under one GIL hold. Verified this doesn't silently drop or duplicate
work (a script that recorded exactly-once execution across n = 0, 1, 5,
100, 1000, 12345, 100000, including an exception-isolation check with a
raising task mixed into a batch - all passed). The trade-off: a worker
now holds the GIL for an entire chunk's worth of Python calls at once,
so less fine-grained interleaving with other Python-level work compared
to one-fiber-per-task. Fine for cheap/uniform tasks (the common case for
this API); something to keep in mind if a batch mixes cheap tasks with
occasional expensive/blocking ones.

Result: n=100k, `spawn_batch_fast` (no-op tasks) went from ~33-43k/s
to **~11,700-11,900k/s** (~280x). Spawn phase alone dropped from
~83-149ms to ~1.1ms since only ~96 fibers get created instead of
100,000.

Verified: 33/33 pytest suite, a dedicated correctness script (exactly-
once execution + exception isolation, described above), 30x repeated
runs of a dedicated chunked-batch stress script (0 failures), a `gdb`
pass, plus the full existing #1/#2/#3 repro-script regression suite
(15x, all synced properly, 0 failures).

#5 not started.
