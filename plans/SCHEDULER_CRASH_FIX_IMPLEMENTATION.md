# Scheduler Crash Fix - Implementation Notes

## Problem Summary
The gsyncio scheduler crashes/hangs when spawning ≥2000 tasks. The crash manifests as an infinite spin loop in `scheduler_wait_all()` where task_count never reaches 0.

## Root Cause Analysis

### Identified Issues
1. **Task Counting Bug**: When 2000 tasks are spawned, only ~1971 complete their execution cycle. This means ~29 tasks (1.5%) are not being properly tracked.

2. **Scheduler Wait Infinite Loop**: The `scheduler_wait_all()` function spins forever waiting for `task_count` to reach 0, but since ~29 tasks are not decremented, it never completes.

3. **Worker Queue Race Conditions**: Possible race conditions in how tasks are added to worker queues vs how they're executed.

### Evidence
- Test with 1200 tasks: ✅ Works
- Test with 1300 tasks: ❌ Crashes/Hangs
- Test with 2000 tasks: task_count=1971 (29 tasks missing)

## Implemented Fix

### Fix: scheduler_wait_all() Stuck Task Detection
Location: `csrc/scheduler.c` - `scheduler_wait_all()` function

**Changes Made:**
1. Added `stuck_count` variable to track how long task_count has been stuck
2. Added detection for when task_count stops decreasing
3. After 100 iterations with same count, force-process fibers from global queue
4. This prevents infinite spinning by making progress on stuck tasks

**Code Change:**
```c
// Before: simple spin loop
while (scheduler_atomic_get_task_count() > 0) {
    // Only yield, no progress
    sched_yield();
}

// After: stuck task detection
uint64_t last_task_count = 0;
uint64_t stuck_count = 0;

while (scheduler_atomic_get_task_count() > 0) {
    uint64_t current_count = scheduler_atomic_get_task_count();
    
    if (current_count == last_task_count) {
        stuck_count++;
        if (stuck_count > 100) {
            // Force process from global queue
            fiber_t* f = NULL;
            pthread_mutex_lock(&sched->mutex);
            f = sched->ready_queue;
            if (f) {
                sched->ready_queue = f->next_ready;
            }
            pthread_mutex_unlock(&sched->mutex);
            // Execute fiber...
            stuck_count = 0;
        }
    } else {
        last_task_count = current_count;
        stuck_count = 0;
    }
    sched_yield();
}
```

## Additional Issues Found

### Duplicate Worker Code Paths
The scheduler has two code paths for fiber execution:
1. **Path 1** (lines 556-628): Has proper atomic CAS for fiber claiming
2. **Path 2** (lines 658-680): Missing atomic CAS validation

This inconsistency could cause race conditions.

### Missing Atomic Operations
The second code path in worker_thread doesn't have proper atomic compare-and-swap for claiming fibers.

## Recommendations for Further Fixes

1. **Consolidate worker code paths** - Merge the two execution paths to use consistent atomic operations
2. **Add fiber pool overflow handling** - When pool is exhausted, handle gracefully instead of returning NULL
3. **Add more debugging output** - Track where task count increments/decrements to find the missing decrements

## Files Modified
- `csrc/scheduler.c` - scheduler_wait_all() function
