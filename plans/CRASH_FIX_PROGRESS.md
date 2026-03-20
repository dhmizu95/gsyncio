# 10K+ Task Crash Fix - Implementation Progress

## Summary

Phase 1 (GIL optimization) and Phase 2 (memory optimization) have been partially implemented. The system now works correctly for up to **1500 tasks** but crashes silently at **2000+ tasks**.

## Implemented Fixes

### Phase 1: GIL Contention Fix ✅

**Changes Made:**
1. Added `sched_yield()` after fiber completion in worker loop
2. Added `sched_yield()` before sleeping in no-work path
3. Optimized `_c_task_entry()` and `_c_fiber_entry()` GIL usage

**Files Modified:**
- `csrc/scheduler.c` - Added yield points at lines 517, 587, 601
- `gsyncio/_gsyncio_core.pyx` - Optimized GIL blocks

**Result:** Improved GIL fairness, reduced contention

### Phase 2: Memory Optimization ✅

**Changes Made:**
1. Reduced initial fiber pool from 64K to 8K fibers
2. Reduced initial timer pool from 64K to 8K timers
3. Kept lazy stack allocation enabled

**Files Modified:**
- `csrc/fiber_pool.c` - `FIBER_POOL_INITIAL_SIZE` = 8192
- `csrc/scheduler.c` - Initial pool sizes reduced

**Result:** Lower initial memory footprint (~10MB vs ~100MB)

## Test Results

| Tasks | Status | Notes |
|-------|--------|-------|
| 100 | ✅ PASS | 0.014s (7,249 tasks/sec) |
| 500 | ✅ PASS | Works reliably |
| 1000 | ✅ PASS | Works reliably |
| 1500 | ✅ PASS | Works reliably |
| 2000 | ❌ CRASH | Silent exit, no error message |
| 10000 | ❌ CRASH | Silent exit |

## Root Cause Analysis: 2000+ Task Crash

### Symptoms
- Silent crash (no segfault, no error message)
- Happens during `gs.sync()` call
- Works with simple functions (`return n`), crashes with `sum(range(100))`

### Hypothesis

**Memory/Stack Exhaustion:**
- Each fiber needs stack space (default 1KB with lazy allocation)
- 2000 fibers × 1KB = 2MB minimum stack
- Plus Python object allocations for `range(100)` and `sum()` results
- Plus scheduler overhead

**Possible Causes:**
1. **Stack overflow** - Fibers hitting stack limits
2. **Memory exhaustion** - Too many simultaneous allocations
3. **Fiber pool growth bug** - Pool not growing correctly under load
4. **Deque overflow** - Worker queues exceeding capacity

### Debug Evidence

```
# Works:
for i in range(1500): gs.task(lambda n: n, 100)  # Simple return

# Crashes:
for i in range(2000): gs.task(lambda n: sum(range(n)), 100)  # Memory intensive
```

## Next Steps

### Immediate (Priority: HIGH)

1. **Add crash diagnostics**
   - Install segfault handler to print stack trace
   - Add memory usage monitoring
   - Log fiber allocation/deallocation

2. **Increase stack size**
   - Try 2KB or 4KB default stack
   - Test if crash moves to higher task count

3. **Check fiber pool growth**
   - Add debug output for pool growth events
   - Verify `fiber_pool_alloc()` handles growth correctly

### Short-term (Priority: MEDIUM)

4. **Implement backpressure**
   - Block `task()` when queue exceeds threshold
   - Return error instead of silent crash

5. **Batch task queuing**
   - Queue tasks in batches of 256
   - Process batches sequentially

### Long-term (Priority: LOW)

6. **Task wrapper object pool**
   - Pre-allocate task wrapper structures
   - Reduce malloc/free overhead

## Files Modified (This Session)

| File | Changes |
|------|---------|
| `csrc/scheduler.c` | Yield points, reduced pool sizes |
| `csrc/fiber_pool.c` | Reduced initial size to 8K |
| `gsyncio/_gsyncio_core.pyx` | GIL optimization |

## Success Criteria (Updated)

- [ ] 2,000 tasks without crash ← **BLOCKING**
- [ ] 10,000 tasks without crash
- [ ] 50,000 tasks without crash
- [ ] 100,000 tasks without crash

## Technical Notes

### Current Configuration
```c
FIBER_POOL_INITIAL_SIZE = 8192      // 8K fibers
FIBER_POOL_MAX_SIZE = 10M           // 10M max
FIBER_DEFAULT_STACK_SIZE = 1024     // 1KB stack
FIBER_POOL_LAZY_STACK = 1           // Lazy allocation
```

### Memory Estimate for 2000 Tasks
```
Fiber control blocks: 2000 × 128 bytes = 256 KB
Fiber stacks: 2000 × 1 KB = 2 MB (lazy allocated)
Python objects: ~100 bytes per task = 200 KB
Scheduler overhead: ~1 MB
Total: ~3.5 MB (well within limits)
```

The crash is NOT due to simple memory exhaustion. More likely a bug in:
- Fiber pool growth logic
- Stack allocation under load
- Deque overflow handling
