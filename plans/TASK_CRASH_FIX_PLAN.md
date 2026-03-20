# 10K+ Task Crash Fix Plan

## Problem Statement
The gsyncio scheduler crashes silently when spawning 10,000+ tasks. The system works correctly for small task counts (~1000) but exhibits silent crashes for larger counts.

## Root Cause Analysis

### Primary Issue: GIL Contention / Scheduler Deadlock
- Worker threads call Python callbacks via `with gil:` block in `_c_task_entry`
- When 10K+ tasks complete simultaneously, workers block waiting for GIL
- This causes scheduler deadlock where no tasks can complete

### Secondary Issues:
1. **Memory Allocation Storms** - Each task spawn creates 2 heap allocations
2. **Lazy Stack Allocation** - mmap storms for fiber stacks under load
3. **No Backpressure** - Tasks queued faster than they can execute

## Implementation Plan

### Phase 1: GIL Contention Fix (Priority: HIGH)
**Goal: Minimize time GIL is held during task execution**

1. **Reduce GIL hold time in callbacks**
   - Location: `gsyncio/_gsyncio_core.pyx` - `_c_task_entry()` function
   - Change: Move Python object extraction outside `with gil:` block
   - Only hold GIL for actual Python function call

2. **Add yield points in worker loop**
   - Location: `csrc/scheduler.c` - worker thread function
   - Add `sched_yield()` after processing each fiber
   - Prevents busy-spin while waiting for GIL

### Phase 2: Memory Optimization (Priority: MEDIUM)
**Goal: Reduce per-task memory allocation overhead**

3. **Implement task wrapper object pool**
   - Location: `csrc/task.c`
   - Pre-allocate 64K task wrapper structures
   - Use lock-free MPMC queue for distribution
   - Avoid malloc/free for every task

4. **Eager fiber pool with configurable size**
   - Location: `csrc/fiber_pool.c`
   - Default to 10K pre-allocated fibers (not 64K)
   - Add runtime configuration for pool size
   - Smaller default stack (512B instead of 8KB)

### Phase 3: Scheduler Improvements (Priority: MEDIUM)
**Goal: Better handling of high concurrency**

5. **Implement batch task queuing**
   - Queue incoming tasks instead of immediate spawn
   - Process in batches of 64-256 per worker tick
   - Reduces scheduler overhead

6. **Add backpressure mechanism**
   - Block task() call when queue exceeds threshold
   - Return error instead of silent crash
   - Allow user to handle overflow

### Phase 4: Diagnostics (Priority: LOW)
**Goal: Better debugging for future issues**

7. **Add crash diagnostic logging**
   - Log at key scheduler points
   - Enable via environment variable
   - Print stack traces on segfault

## Files to Modify

| File | Changes |
|------|---------|
| `gsyncio/_gsyncio_core.pyx` | Optimize GIL usage in `_c_task_entry` |
| `csrc/scheduler.c` | Add yield points, batch processing |
| `csrc/task.c` | Add task wrapper pool |
| `csrc/fiber_pool.c` | Configurable pool size |
| `csrc/fiber.h` | Adjust default stack sizes |

## Success Criteria

- [ ] 10,000 tasks spawn and complete without crash
- [ ] 50,000 tasks spawn and complete without crash  
- [ ] 100,000 tasks spawn and complete without crash
- [ ] No regression in 1,000 task performance

## Timeline

- Phase 1: 2-3 hours
- Phase 2: 2-3 hours
- Phase 3: 3-4 hours
- Phase 4: 1 hour

Total estimated: 8-11 hours

## Alternative Approaches Considered

### Option A: Full Native C Implementation
- Rewrite Python callback system in pure C
- Eliminate GIL entirely for task execution
- **Pros**: Maximum performance
- **Cons**: Large effort, loses Python integration

### Option B: Use asyncio-compatible approach
- Leverage Python's asyncio event loop
- Use selectors/epoll for I/O
- **Pros**: Python ecosystem compatibility
- **Cons**: Different API, may not fit use case

### Option C: Hybrid Approach (Selected)
- Minimize GIL usage in current design
- Add object pools and batch processing
- **Pros**: Incremental improvement, maintains API
- **Cons**: May need further optimization later
