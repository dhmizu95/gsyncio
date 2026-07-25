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

#2-#5 not started.
