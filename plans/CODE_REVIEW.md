# Code Review: gsyncio C Implementation

## Executive Summary

This review identifies **critical issues**, **performance bottlenecks**, and **improvement opportunities** in the gsyncio C codebase. Issues are categorized by severity and potential impact.

---

## Critical Issues (Must Fix)

### 1. Race Condition in Timer Pool

**Location:** [`csrc/scheduler.c:60-70`](csrc/scheduler.c)

```c
static timer_node_t* timer_pool_alloc(void) {
    timer_node_t* node = atomic_load(&g_timer_pool.free_list);
    while (node != NULL) {
        timer_node_t* next = node->next;  // ⚠️ BUG: node->next is NOT atomic!
        if (atomic_compare_exchange_weak(&g_timer_pool.free_list, &node, next)) {
            return node;
        }
        node = atomic_load(&g_timer_pool.free_list);
    }
    // ...
}
```

**Problem:** `node->next` is a regular pointer, but is read without synchronization while being modified concurrently.

**Fix:** Use `atomic_load` or redesign with proper atomic node structure.

---

### 2. Memory Leak in Task Batch Spawn

**Location:** [`csrc/task.c:260-265`](csrc/task.c)

```c
task_wrapper_arg_t* wrapper = (task_wrapper_arg_t*)calloc(1, sizeof(task_wrapper_arg_t));
if (!wrapper) {
    atomic_fetch_sub(&reg->active_count, batch->count - i);  // ⚠️ Missing cleanup!
    return -1;
}
```

**Problem:** On allocation failure, previously allocated handles/fibers are not freed.

**Fix:** Add proper cleanup loop for all allocated resources.

---

### 3. longjmp Without Stack Protection

**Location:** [`csrc/scheduler.c:426, 485`](csrc/scheduler.c)

```c
longjmp(f->context, 1);  // ⚠️ Jumps to saved context without guarantee
```

**Problem:** `longjmp` doesn't update the stack pointer. If the fiber's stack was deallocated (e.g., via fiber pool), this causes undefined behavior / segfault.

**Fix:** Ensure fiber lifetime exceeds context usage or use `setjmp`/`longjmp` more carefully.

---

## High Priority Issues

### 4. Global Atomic Contention

**Location:** [`csrc/scheduler.c:92-100`](csrc/scheduler.c)

```c
uint64_t scheduler_atomic_inc_task_count(void) {
    return __atomic_add_fetch(&g_scheduler->stats.atomic_task_count, 1, __ATOMIC_SEQ_CST);
}
```

**Problem:** 
- `__ATOMIC_SEQ_CST` creates full memory barrier on every operation
- All workers compete for same cache line
- Becomes bottleneck at high concurrency

**Fix:** Implement sharded counters (per-worker local counts).

---

### 5. Inefficient Work Stealing

**Location:** [`csrc/scheduler.c:446-453`](csrc/scheduler.c)

```c
// Check global queue
pthread_mutex_lock(&sched->mutex);
f = sched->ready_queue;
if (f) {
    sched->ready_queue = f->next_ready;
    found_work = 1;
}
pthread_mutex_unlock(&sched->mutex);
```

**Problem:** 
- Global mutex creates contention
- Only checks global queue after local queue is empty
- No attempt to steal from other workers' queues

**Fix:** Implement per-worker deques with lock-free steal operations.

---

### 6. Timer List O(n) Scan

**Location:** [`csrc/evloop.c:57-60`](csrc/evloop.c)

```c
evloop_timer_t *timer = loop->timer_list;
while (timer) {
    // O(n) scan every tick!
    timer = timer->next;
}
```

**Problem:** Linear scan of all timers on every tick doesn't scale to millions of timers.

**Fix:** Implement hierarchical timer wheel (like Linux hrtimer or Netty).

---

### 7. Fixed Stack Size

**Location:** [`csrc/fiber.h:24-27`](csrc/fiber.h)

```c
#define FIBER_INITIAL_STACK_SIZE 1024   // 1KB - too small?
#define FIBER_MAX_STACK_SIZE 32768     // 32KB max
#define FIBER_DEFAULT_STACK_SIZE 2048   // 2KB
```

**Problem:** 
- Every fiber allocates 2KB stack regardless of usage
- 10M fibers = 20GB memory
- No stack growth support

**Fix:** Implement Go-like growing stacks.

---

## Medium Priority Issues

### 8. Missing Error Checks

**Location:** Multiple locations

```c
// csrc/scheduler.c:878
size_t worker_idx = atomic_fetch_add(&g_scheduler->next_worker, 1) % g_scheduler->num_workers;
// ⚠️ Division by zero if num_workers is 0

// csrc/scheduler.c:899
push_local(w, f);  // ⚠️ No check if push_local fails
```

**Fix:** Add null checks and defensive programming.

---

### 9. Spin-Wait Without Backoff

**Location:** [`csrc/scheduler.c:438`](csrc/scheduler.c)

```c
for (int spin = 0; spin < 100 && !w->stopped && !found_work; spin++) {
    __asm__ __volatile__("" ::: "memory");  // ⚠️ Busy-wait
    // ...
}
```

**Problem:** Pure busy-wait wastes CPU cycles. Should add exponential backoff or yield.

---

### 10. Condition Variable Spurious Wakeups

**Location:** [`csrc/task.c:316`](csrc/task.c)

```c
while (atomic_load(&reg->active_count) > 0) {
    pthread_cond_wait(&reg->cond, &reg->mutex);  // ⚠️ Should re-check condition
}
```

**Note:** This is actually correct (while loop), but worth noting for consistency.

---

## Code Quality Issues

### 11. Inconsistent Memory Management

- Some structures use `calloc`, others use `malloc` + `memset`
- Mix of `free` and `munmap`
- No unified memory pool

### 12. Magic Numbers

```c
ts.tv_nsec = 1000000;  // What is this? 1ms?
spin = 100;             // Why 100?
```

**Fix:** Use named constants.

### 13. Missing Documentation

- Many functions lack documentation
- No API contracts (preconditions/postconditions)

---

## Performance Recommendations Summary

| Issue | Current | Recommended | Impact |
|-------|---------|-------------|--------|
| Task counter | Global atomic | Sharded per-worker | 10-100x |
| Timer | O(n) list | O(1) wheel | n-fold |
| Work steal | Global mutex | Per-worker deque | High |
| Stack | Fixed 2KB | Growing | 100x memory |
| Scheduler | Round-robin | Work-stealing | Medium |

---

## Testing Gaps

1. **Concurrency tests:** Multi-threaded stress tests missing
2. **Memory leak tests:** Long-running tests needed
3. **Boundary tests:** 0 workers, max fibers, etc.
4. **Signal handler tests:** longjmp edge cases

---

## Files Requiring Changes

| File | Issues | Priority |
|------|--------|----------|
| csrc/scheduler.c | #1, #4, #5, #8, #9 | Critical |
| csrc/task.c | #2, #8 | High |
| csrc/evloop.c | #6 | High |
| csrc/fiber.h/c | #3, #7 | Medium |
| csrc/fiber_pool.c | #11 | Low |

---

## Recommended Action Plan

1. **Immediate (Critical):**
   - Fix race condition in timer pool (#1)
   - Add cleanup in task batch spawn (#2)

2. **Short-term (High):**
   - Implement sharded counters (#4)
   - Implement work-stealing deques (#5)
   - Add timer wheel (#6)

3. **Medium-term:**
   - Growing stacks (#7)
   - Error handling (#8)
   - Spin-wait optimization (#9)
