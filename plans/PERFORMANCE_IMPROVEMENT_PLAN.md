# gsyncio Performance Improvement Plan

## Current Performance Gaps

| Benchmark | Go | gsyncio | Gap | Target |
|-----------|-----|---------|-----|--------|
| Task Spawn (1000) | 0.53ms | 68.94ms | **130x** | <10ms |
| WaitGroup (10×100) | 0.07ms | 2.16ms | **31x** | <0.5ms |
| Context Switch | 0.21µs | 0.14µs | ✅ **1.5x faster** | Maintain |

---

## Root Cause Analysis

### 1. Task Spawn Overhead (130x slower than Go)

**Current bottlenecks:**
```python
# Python task.py wrapper adds overhead
def task(func, *args, **kwargs):
    def wrapper():  # ← Extra Python function call
        try:
            func(*args, **kwargs)  # ← Python argument unpacking
        except Exception as e:  # ← Exception handling overhead
            ...
    _spawn(wrapper)  # ← Python object creation
```

**Overhead breakdown (estimated):**
- Python function wrapper: ~500ns
- Argument unpacking (*args, **kwargs): ~200ns
- Exception handling setup: ~100ns
- Python object allocation (tuple for payload): ~300ns
- GIL acquisition/release: ~100ns
- **Total Python overhead: ~1.2µs per task**

**For 1000 tasks: ~1.2ms just in Python overhead**

### 2. WaitGroup Operations (31x slower than Go)

**Current bottlenecks:**
```python
# Python threading.Condition uses mutex + futex
with _tasks_lock:  # ← Python context manager
    _pending_count -= 1
    if _pending_count == 0:
        _all_done_event.set()  # ← Python method call
```

**Overhead breakdown:**
- Python lock acquisition: ~200ns
- Python attribute access: ~50ns
- Python integer operations: ~20ns
- Event set() call: ~100ns
- **Total per done() call: ~370ns**

**For 1000 operations: ~370µs just in Python overhead**

### 3. Thread Scheduling Overhead

**Current architecture:**
```
Python task() → Cython spawn() → C scheduler_spawn() → Worker threads
                ↓
           GIL acquired for Python callback
```

**Issues:**
- Worker threads are OS threads (expensive context switches)
- Python's GIL creates contention
- No fiber pooling in Python layer
- Each task creates Python objects

---

## Improvement Strategies

### Phase 1: Reduce Python Overhead (Target: 10x improvement)

#### 1.1 Batch Task Spawning

**Current:**
```python
for i in range(1000):
    gs.task(worker)  # 1000 Python → C calls
```

**Improved:**
```python
# Spawn all tasks in single call
gs.spawn_batch([worker]*1000)  # 1 Python → C call
```

**Implementation:**
```c
// C implementation - no Python loop overhead
int scheduler_spawn_batch(void (*entry)(void*), void** args, size_t count) {
    pthread_mutex_lock(&sched->mutex);
    for (size_t i = 0; i < count; i++) {
        fiber_t* f = fiber_pool_alloc(sched->fiber_pool);
        f->func = entry;
        f->arg = args[i];
        push_local(worker, f);
    }
    pthread_cond_broadcast(&sched->cond);
    pthread_mutex_unlock(&sched->mutex);
    return 0;
}
```

**Expected improvement:** 5-10x for bulk spawning

#### 1.2 Eliminate Python Wrapper Functions

**Current:**
```python
def task(func, *args, **kwargs):
    def wrapper():
        func(*args, **kwargs)
    _spawn(wrapper)
```

**Improved:**
```python
# Direct C function that stores func/args in struct
cdef class TaskPayload:
    cdef object func
    cdef tuple args
    
cdef void _c_task_entry(void* arg) noexcept nogil:
    with gil:
        payload = <TaskPayload>arg
        try:
            payload.func(*payload.args)
        except:
            pass
```

**Expected improvement:** 2-3x

#### 1.3 Object Pooling

**Current:** Every task creates new Python objects

**Improved:**
```python
# Pre-allocate payload objects
_payload_pool = []
for _ in range(10000):
    _payload_pool.append(TaskPayload())

def task(func, *args):
    payload = _payload_pool.pop()
    payload.func = func
    payload.args = args
    _spawn_with_payload(payload)
```

**Expected improvement:** 2x for object creation

---

### Phase 2: Optimize C Layer (Target: 5x improvement)

#### 2.1 Lock-Free Task Queue

**Current:** Mutex-protected ready queue

**Improved:** Lock-free MPMC queue
```c
typedef struct lockfree_queue {
    _Atomic fiber_t* head;
    _Atomic fiber_t* tail;
} lockfree_queue_t;

int lockfree_push(lockfree_queue_t* q, fiber_t* f) {
    fiber_t* null_fiber = NULL;
    f->next_ready = NULL;
    
    while (!atomic_compare_exchange_weak(&q->tail, &null_fiber, f)) {
        f->next_ready = null_fiber;
    }
    return 0;
}
```

**Expected improvement:** 2-3x for concurrent spawning

#### 2.2 Per-Worker Fiber Pools

**Current:** Single global fiber pool with mutex

**Improved:** Per-worker pools (no contention)
```c
typedef struct worker {
    fiber_pool_t* local_pool;  // No mutex needed
    deque_t* ready_queue;
    // ...
} worker_t;

fiber_t* fiber_alloc_local(worker_t* w) {
    return fiber_pool_alloc(w->local_pool);  // Lock-free
}
```

**Expected improvement:** 3-5x for high-concurrency workloads

#### 2.3 Optimized WaitGroup in C

**Current:** Python threading.Condition

**Improved:** C implementation with futex
```c
typedef struct waitgroup {
    _Atomic int64_t counter;
    _Atomic fiber_t* waiters;
} waitgroup_t;

void waitgroup_done(waitgroup_t* wg) {
    int64_t old = atomic_fetch_sub(&wg->counter, 1);
    if (old == 1) {
        // Wake all waiters
        fiber_t* waiters = atomic_exchange(&wg->waiters, NULL);
        while (waiters) {
            fiber_t* next = waiters->next_ready;
            fiber_unpark(waiters);
            waiters = next;
        }
    }
}
```

**Expected improvement:** 5-10x

---

### Phase 3: Architecture Improvements (Target: 3x improvement)

#### 3.1 True Fiber Implementation

**Current:** setjmp/longjmp with mmap'd stacks

**Issues:**
- Stack allocation is expensive (mmap: ~1µs)
- No stack copying for work-stealing
- Fixed stack size (wasteful)

**Improved:** Segmented stacks (like Go)
```c
typedef struct fiber {
    void* stack_segment;
    size_t stack_size;
    struct fiber* stack_next;  // For segmented stacks
    // ...
} fiber_t;

void fiber_grow_stack(fiber_t* f) {
    // Allocate smaller segment (4KB instead of 2MB)
    void* new_segment = mmap(NULL, 4096, ...);
    // Link to current stack
    ((stack_header_t*)new_segment)->next = f->stack_segment;
    f->stack_segment = new_segment;
}
```

**Expected improvement:** 10x for stack allocation, 2x memory efficiency

#### 3.2 Inline Cache for Python Calls

**Current:** Every fiber execution acquires GIL

**Improved:** Cache function type and call directly
```c
typedef struct cached_call {
    void* func_ptr;      // C function pointer
    PyObject* py_func;   // Python function (for fallback)
    int is_c_function;   // Fast path flag
} cached_call_t;

void fiber_run(fiber_t* f) {
    cached_call_t* cache = f->call_cache;
    
    if (cache->is_c_function) {
        // No GIL needed!
        ((void(*)(void*))cache->func_ptr)(f->arg);
    } else {
        // Acquire GIL for Python call
        with gil:
            cache->py_func(f->arg);
    }
}
```

**Expected improvement:** 5-10x for C extensions, 1.5x for Python

#### 3.3 Batched Context Switching

**Current:** Switch on every yield

**Improved:** Cooperative batching
```c
// Don't switch immediately, mark for later
void fiber_yield_batched(fiber_t* f) {
    f->state = FIBER_READY;
    f->yield_pending = 1;
    
    // Only actually yield if budget exhausted
    if (--f->time_slice <= 0) {
        fiber_yield_real(f);
    }
}
```

**Expected improvement:** 2-3x for tight loops

---

### Phase 4: Advanced Optimizations (Target: 2x improvement)

#### 4.1 CPU Pinning and NUMA Awareness

```c
void worker_init(worker_t* w) {
    // Pin to specific CPU
    cpu_set_t cpuset;
    CPU_SET(w->cpu_id, &cpuset);
    pthread_setaffinity_np(w->thread, sizeof(cpuset), &cpuset);
    
    // Allocate memory from local NUMA node
    w->local_pool = numa_alloc_onnode(..., w->numa_node);
}
```

**Expected improvement:** 1.5-2x for memory-bound workloads

#### 4.2 Speculative Execution

```c
// Pre-allocate fibers before they're needed
void scheduler_prefetch_fibers(scheduler_t* sched) {
    if (fiber_pool_available(sched->fiber_pool) < 100) {
        // Pre-allocate in background
        spawn_background_task(fiber_pool_grow, sched->fiber_pool);
    }
}
```

**Expected improvement:** 1.2-1.5x

#### 4.3 Adaptive Work-Stealing

**Current:** Random victim selection

**Improved:** Steal from busiest worker
```c
int select_victim_adaptive(worker_t* thief) {
    size_t max_work = 0;
    int victim = -1;
    
    for (size_t i = 0; i < num_workers; i++) {
        size_t work = atomic_load(&workers[i].queue_size);
        if (work > max_work + THRESHOLD) {
            max_work = work;
            victim = i;
        }
    }
    return victim;
}
```

**Expected improvement:** 1.5-2x for uneven workloads

---

## Implementation Priority

### Immediate (Week 1-2): 10x improvement expected
1. ✅ Batch task spawning (Phase 1.1)
2. ✅ Eliminate Python wrappers (Phase 1.2)
3. ✅ Object pooling (Phase 1.3)

### Short-term (Week 3-4): 5x improvement expected
4. ✅ Lock-free queues (Phase 2.1)
5. ✅ Per-worker fiber pools (Phase 2.2)
6. ✅ C WaitGroup (Phase 2.3)

### Medium-term (Month 2): 3x improvement expected
7. ⏳ Segmented stacks (Phase 3.1)
8. ⏳ Inline caching (Phase 3.2)
9. ⏳ Batched yielding (Phase 3.3)

### Long-term (Month 3+): 2x improvement expected
10. ⏳ CPU pinning/NUMA (Phase 4.1)
11. ⏳ Speculative execution (Phase 4.2)
12. ⏳ Adaptive stealing (Phase 4.3)

---

## Expected Final Performance

| Benchmark | Current | After Phase 1-2 | After Phase 3-4 | Target |
|-----------|---------|-----------------|-----------------|--------|
| Task Spawn | 68.94ms | ~1ms | ~0.3ms | <1ms |
| WaitGroup | 2.16ms | ~0.2ms | ~0.1ms | <0.1ms |
| Context Switch | 0.14µs | 0.10µs | 0.08µs | <0.1µs |

**Overall improvement: 50-100x**

---

## Quick Wins (Can be implemented in 1-2 days)

1. **Remove exception handling from hot path:**
   ```python
   # Only catch exceptions in debug mode
   #ifdef DEBUG
   try: func(*args)
   except: log_error()
   #else
   func(*args)  # No try/except overhead
   #endif
   ```

2. **Use __slots__ for Python classes:**
   ```python
   class TaskPayload:
       __slots__ = ['func', 'args']  # 30% faster attribute access
   ```

3. **Pre-allocate tuples for args:**
   ```python
   # Instead of (func, args) tuple creation each time
   payload = _payload_pool.pop()
   payload.func = func
   payload.args = args
   ```

4. **Use Cython's cpdef for hot functions:**
   ```cython
   cpdef void scheduler_schedule(fiber_t* f) nogil:
       # Can be called from both Python and C
   ```

---

## Measurement and Validation

For each optimization:
1. Run benchmark before/after
2. Profile with `perf` to identify bottlenecks
3. Check for regressions in other benchmarks
4. Validate correctness with stress tests

```bash
# Profiling commands
perf record -g python3 benchmark_gsyncio.py
perf report --stdio

# Memory profiling
valgrind --tool=massif python3 benchmark_gsyncio.py
```
