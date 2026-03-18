# gsyncio Performance Improvement Plan

## Executive Summary

This document outlines a comprehensive plan to achieve the target performance goals stated in the gsyncio README:
- **Context Switch Time**: <1 µs (currently ~50-100 µs via asyncio fallback)
- **Memory per Task**: <200 bytes (currently ~50 KB due to asyncio overhead)
- **Max Concurrent Tasks**: 1M+ (currently ~100K practical limit)
- **I/O Throughput**: 2-10x improvement over asyncio
- **HTTP Server RPS**: 100K+ target

---

## Current State Analysis

### Architecture Overview

```
Python Application
    ↓
gsyncio Python Layer (task.py, async_.py, channel.py, etc.)
    ↓
Cython Wrapper (_gsyncio_core.pyx)
    ↓
gsyncio C Core (fiber.c, scheduler.c, evloop.c, etc.)
    ↓
Linux Kernel (epoll/io_uring)
```

### Identified Performance Bottlenecks

1. **Async Operations Fall Back to asyncio**
   - `sleep_ms()` delegates to asyncio event loop
   - No native async I/O path in C extension
   - Python GIL contention in async path

2. **Fiber Context Switching Overhead**
   - Uses `sigsetjmp`/`siglongjmp` (saves signal mask - slow)
   - No fiber pooling in hot path
   - Stack allocation via `mmap` (slow, no reuse)

3. **Scheduler Inefficiencies**
   - Global mutex contention for cross-worker scheduling
   - Work-stealing uses random victim selection (suboptimal)
   - No batch scheduling for spawned fibers

4. **I/O Path Issues**
   - io_uring integration incomplete (not enabled by default)
   - FD table lookup is O(1) but has mutex contention
   - Timer operations use global mutex

5. **Memory Allocation**
   - Fiber allocation: `calloc` + `mmap` per fiber
   - No slab allocator for common sizes
   - Channel/send-recv allocate on every operation

---

## Phase 1: Fiber Performance (Week 1-2)

### 1.1 Replace `sigsetjmp` with `setjmp`

**Problem**: `sigsetjmp` saves signal mask (~100-200 cycles overhead)

**Solution**: Use plain `setjmp`/`longjmp` since we don't need signal mask preservation

```c
// Before (fiber.h)
sigjmp_buf context;

// After
jmp_buf context;

// Before (fiber.c)
if (sigsetjmp(fiber->context, 1) == 0) {
    siglongjmp(to->context, 1);
}

// After
if (setjmp(fiber->context) == 0) {
    longjmp(to->context, 1);
}
```

**Expected Improvement**: 20-30% faster context switches

### 1.2 Implement Slab Allocator for Fibers

**Problem**: `calloc` + `mmap` per fiber is slow (~5-10 µs)

**Solution**: Pre-allocate fiber slabs with common stack sizes

```c
// New: fiber_slab.h
typedef struct fiber_slab {
    void** free_list;
    size_t slab_size;
    size_t free_count;
    struct fiber_slab* next;
} fiber_slab_t;

typedef struct fiber_allocator {
    fiber_slab_t* slabs[4];  // 2KB, 4KB, 8KB, 16KB
    pthread_mutex_t lock;
} fiber_allocator_t;

fiber_t* fiber_alloc(fiber_allocator_t* alloc, size_t stack_size);
void fiber_free(fiber_allocator_t* alloc, fiber_t* fiber);
```

**Expected Improvement**: 10x faster allocation (500ns vs 5µs)

### 1.3 Add Fiber Pool to Hot Path

**Problem**: `fiber_pool_alloc` exists but not used consistently

**Solution**: Make fiber pool the default allocation path

```c
// scheduler_spawn - use pool first
fiber_t* f = fiber_pool_alloc(sched->fiber_pool);
if (!f) {
    f = fiber_create(entry, user_data, stack_size);
}
```

**Expected Improvement**: 5x faster task spawning

---

## Phase 2: Scheduler Optimization (Week 2-3)

### 2.1 Lock-Free Work-Stealing

**Problem**: Global mutex contention when scheduling across workers

**Solution**: Implement lock-free deque (Chase-Lev style)

```c
// Use atomic operations for deque
typedef struct lock_free_deque {
    _Atomic size_t top;
    _Atomic size_t bottom;
    _Atomic fiber_t** data;
    size_t capacity;
    _Atomic size_t capacity_mask;
} lock_free_deque_t;

// Lock-free push (owner only)
void lf_push_top(lock_free_deque_t* dq, fiber_t* f) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    
    if (b - t >= dq->capacity) {
        // Resize (rare)
        return;
    }
    
    dq->data[b & dq->capacity_mask] = f;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
}

// Lock-free steal (thieves)
fiber_t* lf_steal(lock_free_deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    
    if (t >= b) return NULL;
    
    fiber_t* f = dq->data[t & dq->capacity_mask];
    if (atomic_compare_exchange_weak(&dq->top, &t, t + 1)) {
        return f;
    }
    return NULL;  // Lost race, retry
}
```

**Expected Improvement**: 3x better multi-threaded scaling

### 2.2 Batch Scheduling

**Problem**: Each `scheduler_spawn` acquires mutex individually

**Solution**: Batch multiple spawns into single mutex acquisition

```c
typedef struct spawn_batch {
    fiber_t** fibers;
    size_t count;
    size_t capacity;
} spawn_batch_t;

void scheduler_spawn_batch(spawn_batch_t* batch, int worker_id) {
    worker_t* w = &sched->workers[worker_id];
    
    // Single mutex lock for entire batch
    for (size_t i = 0; i < batch->count; i++) {
        push_top(w->deque, batch->fibers[i]);
    }
    
    // Single signal
    pthread_cond_signal(&sched->cond);
}
```

**Expected Improvement**: 10x faster bulk task creation

### 2.3 NUMA-Aware Worker Placement

**Problem**: All workers compete for same memory bandwidth

**Solution**: Pin workers to CPU cores, allocate fibers from local NUMA node

```c
void worker_init_numa(worker_t* w) {
    int cpu = w->id % get_num_cpus();
    
    // Pin thread to CPU
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(w->thread, sizeof(cpuset), &cpuset);
    
    // Set NUMA memory policy (Linux-specific)
    #ifdef __linux__
    set_mempolicy(MPOL_PREFERRED, NULL, 0);
    #endif
}
```

**Expected Improvement**: 20-30% better cache locality

---

## Phase 3: Native I/O Path (Week 3-5)

### 3.1 Enable io_uring by Default

**Problem**: io_uring exists but not enabled/used

**Solution**: Make io_uring the default I/O backend on Linux 5.1+

```c
// scheduler_init
#ifdef __linux__
    // Try io_uring first, fall back to epoll
    if (io_uring_queue_init(entries, &sched->io_uring_ring, 0) == 0) {
        sched->io_uring_enabled = true;
        sched->backend = SCHEDULER_BACKEND_IOURING;
    } else {
        sched->io_uring_enabled = false;
        sched->backend = SCHEDULER_BACKEND_EPOLL;
    }
#endif
```

**Expected Improvement**: 2-5x I/O throughput

### 3.2 Implement Native Async Sleep

**Problem**: `sleep_ms()` delegates to asyncio (Python GIL contention)

**Solution**: Native timer wheel in C

```c
// timer_wheel.h
typedef struct timer_wheel {
    timer_node_t** wheels[64];  // 64 slots, 1ms resolution
    uint64_t current_tick;
    pthread_mutex_t lock;
} timer_wheel_t;

uint64_t timer_wheel_add(timer_wheel_t* tw, uint64_t delay_ms, fiber_t* fiber);
void timer_wheel_tick(timer_wheel_t* tw);
```

```c
// In scheduler main loop
void scheduler_process_timers(scheduler_t* sched) {
    uint64_t now = get_time_ns();
    
    pthread_mutex_lock(&sched->timers_mutex);
    timer_node_t** prev = &sched->timers;
    timer_node_t* node = sched->timers;
    
    while (node) {
        if (node->deadline_ns <= now) {
            *prev = node->next;
            
            // Wake fiber
            node->fiber->state = FIBER_READY;
            scheduler_schedule(node->fiber, -1);
            
            timer_node_t* to_free = node;
            node = node->next;
            free(to_free);
        } else {
            prev = &node->next;
            node = node->next;
        }
    }
    pthread_mutex_unlock(&sched->timers_mutex);
}
```

**Expected Improvement**: 100x faster sleep (1 µs vs 100 µs)

### 3.3 Socket I/O Integration

**Problem**: No native socket send/recv

**Solution**: io_uring-based async socket operations

```c
// In channel.c for network channels
async_result_t channel_send_network(channel_t* ch, void* value) {
    fiber_t* current = fiber_current();
    
    io_request_t req = {
        .fiber = current,
        .fd = ch->network_fd,
        .op = IO_OP_SEND,
        .buf = value,
        .len = ch->msg_size,
    };
    
    current->state = FIBER_WAITING;
    current->waiting_on = &req;
    
    scheduler_submit_io(&req);
    fiber_yield();  // Wait for completion
    
    return req.completed ? OK : ERROR;
}
```

**Expected Improvement**: 5x network throughput

---

## Phase 4: Memory Optimization (Week 5-6)

### 4.1 Reduce Fiber Memory Footprint

**Problem**: Fiber struct is ~200 bytes but stack is 2KB minimum

**Solution**: Separate hot/cold fields, use smaller initial stack

```c
// Split fiber into hot (frequently accessed) and cold (rarely accessed)
typedef struct fiber_hot {
    uint64_t id;
    fiber_state_t state;
    void* stack_ptr;
    void (*func)(void*);
    fiber_t* next_ready;
    jmp_buf context;
} __attribute__((aligned(64))) fiber_hot_t;  // Cache-line aligned

typedef struct fiber_cold {
    void* stack_base;
    size_t stack_size;
    void* arg;
    void* result;
    fiber_t* parent;
    const char* name;
    void* waiting_on;
} fiber_cold_t;

struct fiber {
    fiber_hot_t hot;
    fiber_cold_t cold;
};
```

**Expected Improvement**: 50% better cache utilization

### 4.2 Object Pool for Channels

**Problem**: Every channel operation allocates memory

**Solution**: Pre-allocate channel buffers

```c
typedef struct channel_pool {
    channel_t** channels;
    size_t capacity;
    size_t available;
    pthread_mutex_t lock;
} channel_pool_t;

channel_t* channel_pool_get(channel_pool_t* pool, size_t capacity) {
    pthread_mutex_lock(&pool->lock);
    if (pool->available == 0) {
        // Expand pool
    }
    channel_t* ch = pool->channels[--pool->available];
    pthread_mutex_unlock(&pool->lock);
    
    ch->capacity = capacity;
    ch->size = 0;
    ch->closed = 0;
    return ch;
}
```

**Expected Improvement**: 3x faster channel operations

### 4.3 Stack Size Auto-Tuning

**Problem**: Fixed 2KB stack wastes memory for simple tasks

**Solution**: Start with 512 bytes, grow on demand

```c
#define FIBER_MIN_STACK 512
#define FIBER_MAX_STACK 32768

void fiber_grow_if_needed(fiber_t* f) {
    if (f->stack_size < FIBER_MAX_STACK) {
        size_t new_size = f->stack_size * 2;
        void* new_stack = mmap(..., new_size, ...);
        
        // Copy old stack to new
        memcpy(new_stack + new_size - f->stack_size,
               f->stack_base + f->stack_size - f->stack_size,
               f->stack_size);
        
        munmap(f->stack_base, f->stack_capacity);
        f->stack_base = new_stack;
        f->stack_size = new_size;
    }
}
```

**Expected Improvement**: 4x memory efficiency for simple tasks

---

## Phase 5: Python Integration (Week 6-7)

### 5.1 Reduce Python C-API Overhead

**Problem**: Every fiber operation crosses Python/C boundary

**Solution**: Batch Python callbacks, use fast paths

```c
// In _gsyncio_core.pyx
cdef class Future:
    # Cache Python methods
    cdef object _set_result_cb
    
    def __cinit__(self):
        self._set_result_cb = self.set_result
    
    # Fast path for C->Python callback
    cdef void complete_with_result(self, void* result):
        # Direct call without Python lookup
        self.set_result(<object>result)
```

**Expected Improvement**: 20% reduction in Python overhead

### 5.2 GIL-Free Scheduler Operations

**Problem**: Scheduler holds GIL during fiber switches

**Solution**: Release GIL during pure C operations

```c
void scheduler_run_without_gil() {
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    while (sched->running) {
        // Release GIL during fiber execution
        Py_BEGIN_ALLOW_THREADS
        
        fiber_t* f = pop_local(&sched->workers[0]);
        if (f) {
            // Execute fiber without GIL
            f->func(f->arg);
        }
        
        Py_END_ALLOW_THREADS
        
        // Re-acquire GIL for Python operations
        if (needs_python_callback) {
            // Handle Python callbacks
        }
    }
    
    PyGILState_Release(gstate);
}
```

**Expected Improvement**: 2x better multi-threaded performance

### 5.3 Zero-Copy Channel Messages

**Problem**: Channel send/recv copies message data

**Solution**: Pass Python object references (with refcount)

```c
int channel_send_zero_copy(channel_t* ch, PyObject* obj) {
    // Just increment refcount, don't copy
    Py_INCREF(obj);
    
    if (ch->size < ch->capacity) {
        ch->buffer[ch->tail] = obj;
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->size++;
        return 0;
    }
    
    // Buffer full, block
    Py_DECREF(obj);
    return -1;
}

PyObject* channel_recv_zero_copy(channel_t* ch) {
    if (ch->size > 0) {
        PyObject* obj = ch->buffer[ch->head];
        ch->head = (ch->head + 1) % ch->capacity;
        ch->size--;
        return obj;  // Caller owns reference
    }
    return NULL;
}
```

**Expected Improvement**: 5x faster channel throughput

---

## Phase 6: Advanced Optimizations (Week 7-8)

### 6.1 Adaptive Work-Stealing

**Problem**: Random victim selection is inefficient

**Solution**: Track worker load, steal from busiest

```c
typedef struct worker_stats {
    _Atomic size_t deque_size;
    _Atomic uint64_t last_steal_time;
    _Atomic uint64_t tasks_completed;
} worker_stats_t;

int select_victim(worker_t* thief) {
    size_t max_size = 0;
    int victim = -1;
    
    for (size_t i = 0; i < g_scheduler->num_workers; i++) {
        if (i == thief->id) continue;
        
        size_t size = atomic_load(&g_scheduler->workers[i].stats.deque_size);
        if (size > max_size && size > 2) {  // Only steal if > 2 tasks
            max_size = size;
            victim = i;
        }
    }
    
    return victim;
}
```

**Expected Improvement**: 30% better load balancing

### 6.2 Fiber Affinity Hints

**Problem**: Fibers bounce between workers (cache pollution)

**Solution**: Track CPU affinity, schedule on same worker

```c
void scheduler_schedule_with_affinity(fiber_t* f, int preferred_worker) {
    if (preferred_worker >= 0 && 
        !deque_empty(&g_scheduler->workers[preferred_worker].deque)) {
        push_local(&g_scheduler->workers[preferred_worker], f);
    } else {
        // Find worker with smallest queue
        size_t min_size = SIZE_MAX;
        int target = 0;
        
        for (size_t i = 0; i < g_scheduler->num_workers; i++) {
            size_t size = g_scheduler->workers[i].deque->top - 
                         g_scheduler->workers[i].deque->bottom;
            if (size < min_size) {
                min_size = size;
                target = i;
            }
        }
        
        push_local(&g_scheduler->workers[target], f);
    }
}
```

**Expected Improvement**: 20% better cache hit rate

### 6.3 Speculative Fiber Preemption

**Problem**: Long-running fibers starve others

**Solution**: Cooperative preemption with yield injection

```c
#define FIBER_YIELD_INTERVAL 1000  // Yield every 1000 instructions

void fiber_check_preemption() {
    static _Thread_local uint64_t instruction_count = 0;
    
    instruction_count++;
    if (instruction_count >= FIBER_YIELD_INTERVAL) {
        instruction_count = 0;
        fiber_yield();
    }
}

// Insert at function prologue (via compiler instrumentation or manual)
#define FIBER_ENTRY() fiber_check_preemption()
```

**Expected Improvement**: Better latency for high-priority fibers

---

## Benchmarking Strategy

### Micro-Benchmarks

| Benchmark | Current | Target | Metric |
|-----------|---------|--------|--------|
| Context Switch | 50-100 µs | <1 µs | `benchmark_context_switch()` |
| Task Spawn | 10 µs | <500 ns | `benchmark_task_spawn()` |
| Sleep | 100 µs | <10 µs | `benchmark_sleep()` |
| Channel Send/Recv | 50 µs | <5 µs | `benchmark_channel()` |
| WaitGroup | 20 µs | <2 µs | `benchmark_waitgroup()` |

### Macro-Benchmarks

| Benchmark | Current | Target | Metric |
|-----------|---------|--------|--------|
| HTTP Echo Server | 20K RPS | 100K+ RPS | `wrk -t4 -c1000` |
| Concurrent Connections | 10K | 1M+ | Active connections |
| Memory per Connection | 50 KB | <200 bytes | RSS / connections |
| I/O Throughput | 100 MB/s | 1+ GB/s | `dd` over network |

### Comparison Benchmarks

| Framework | Target Ratio | Metric |
|-----------|--------------|--------|
| gsyncio vs asyncio | 2-10x | Same workload |
| gsyncio vs Go | <2x | Context switch time |
| gsyncio vs uvloop | 1-2x | HTTP RPS |

---

## Implementation Timeline

| Phase | Duration | Key Deliverables |
|-------|----------|------------------|
| 1. Fiber Performance | Week 1-2 | `setjmp`, slab allocator, fiber pool |
| 2. Scheduler Optimization | Week 2-3 | Lock-free deque, batch scheduling |
| 3. Native I/O Path | Week 3-5 | io_uring default, native sleep, sockets |
| 4. Memory Optimization | Week 5-6 | Hot/cold split, object pools |
| 5. Python Integration | Week 6-7 | GIL-free ops, zero-copy channels |
| 6. Advanced Optimizations | Week 7-8 | Adaptive stealing, affinity hints |

---

## Risk Mitigation

### Technical Risks

1. **io_uring Compatibility**
   - Risk: io_uring not available on older kernels
   - Mitigation: Fallback to epoll, runtime detection

2. **Lock-Free Data Structures**
   - Risk: ABA problem, memory ordering bugs
   - Mitigation: Extensive testing with ThreadSanitizer

3. **Stack Overflow**
   - Risk: Smaller initial stacks may overflow
   - Mitigation: Guard pages, automatic growth, monitoring

### Testing Strategy

1. **Unit Tests**: Each optimization has dedicated tests
2. **Stress Tests**: Run with 1M+ fibers for 24+ hours
3. **Leak Detection**: Valgrind, AddressSanitizer
4. **Race Detection**: ThreadSanitizer, Helgrind
5. **Performance Regression**: CI with benchmark comparison

---

## Success Criteria

### Phase 1-2 (Core Performance)
- [ ] Context switch <5 µs
- [ ] Task spawn <1 µs
- [ ] Memory per fiber <1 KB

### Phase 3-4 (I/O and Memory)
- [ ] Native sleep <10 µs
- [ ] Channel ops <10 µs
- [ ] Memory per task <200 bytes

### Phase 5-6 (Production Ready)
- [ ] HTTP server 100K+ RPS
- [ ] 1M+ concurrent connections
- [ ] Zero memory leaks (24h stress test)
- [ ] No race conditions (ThreadSanitizer clean)

---

## Conclusion

This plan outlines a systematic approach to achieving gsyncio's performance goals. The key insights are:

1. **Eliminate Python overhead** in hot paths (GIL, asyncio fallback)
2. **Use lock-free data structures** for multi-threaded scaling
3. **Leverage io_uring** for high-performance I/O
4. **Optimize memory allocation** with pools and slab allocators
5. **Improve cache locality** with affinity and NUMA awareness

Expected final performance:
- **Context Switch**: 0.5-1 µs (100x improvement)
- **Task Spawn**: 200-500 ns (20x improvement)
- **I/O Throughput**: 1-2 GB/s (10x improvement)
- **HTTP Server**: 100K-200K RPS (5-10x improvement)

This positions gsyncio as a viable alternative to Go for high-concurrency Python applications.
