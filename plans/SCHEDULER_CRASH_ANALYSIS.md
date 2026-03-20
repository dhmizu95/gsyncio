# Scheduler Crash Analysis - 2000+ Task Crash

## Summary
The scheduler crashes when spawning ≥2000 tasks. Based on code analysis, several potential root causes have been identified.

## Identified Issues

### 1. Duplicate Worker Code Paths (CRITICAL)
Location: `csrc/scheduler.c` lines 556-628 and 658-680

The worker loop has two code paths for fiber execution:
- **Path 1** (lines 556-628): Has proper atomic CAS for fiber claiming
- **Path 2** (lines 658-680): Missing atomic CAS, uses simple state check

This inconsistency can cause race conditions where:
- Two workers claim the same fiber
- Fiber state changes between check and execution
- Use-after-free when fiber is freed while another worker is executing it

### 2. Missing Fiber State Validation in Path 2
```c
// Path 1 - Has proper checking
fiber_state_t expected_state = f->state;
if (expected_state != FIBER_NEW && expected_state != FIBER_READY) {
    /* Fiber already being processed by another worker */
    ...
}
if (!__atomic_compare_exchange_n(&f->state, &expected_state, FIBER_RUNNING, ...))

// Path 2 - Missing validation
if (f->state == FIBER_NEW || f->state == FIBER_READY) {
    // No atomic CAS - race condition!
```

### 3. Potential Task Count Underflow
Location: `csrc/scheduler.c` lines 598-600, 671-673

Both atomic and sharded counters are decremented for each completed task. If there's a race condition where the same task is "completed" twice, the counter could go negative.

### 4. Fiber Pool Growth Race Condition
Location: `csrc/fiber_pool.c` lines 231-280

When the fiber pool is exhausted, multiple workers might try to grow the pool simultaneously. The current implementation uses a mutex but there might be race conditions in the allocation path.

### 5. NULL Stack Dereference
Location: `csrc/scheduler.c` lines 578-583

There's a check for NULL stack which indicates lazy stack allocation might not be working correctly in some edge cases:
```c
if (!f->stack_base && !f->stack_ptr) {
    fprintf(stderr, "[ERROR] Worker %d: Fiber %lu has NULL stack! ...
}
```

## Proposed Fixes

### Fix 1: Consolidate Worker Code Paths
Merge the two code paths in the worker loop to use consistent atomic operations for fiber claiming.

### Fix 2: Add Atomic CAS to Second Path
Add the missing `__atomic_compare_exchange_n` to the second code path.

### Fix 3: Add Task Count Validation
Add validation to prevent task count from going negative.

### Fix 4: Add Debug Logging
Add more debug logging to trace the crash location.

## Test Plan
1. Enable GSYNCIO_DEBUG=1 and run 2000 task test
2. Capture stack trace from crash
3. Implement fixes
4. Verify with 2000, 5000, 10000 task tests
5. Run performance benchmarks to ensure no regression

## Files to Modify
- `csrc/scheduler.c` - Fix worker loop race conditions
- `csrc/fiber_pool.c` - Add safety checks for pool growth
