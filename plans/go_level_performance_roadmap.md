# Roadmap to Go-Level Performance for gsyncio

## Current State vs Go Performance

| Metric | gsyncio Current | Go | Gap |
|--------|-----------------|-----|-----|
| Context Switch | 1.08 µs | 0.5 µs | 2.2x slower |
| Task Creation | 500-800 ns* | 200-500 ns | 1.6-2x slower |
| Memory/Task | 2.2 KB | 8.5 KB | ✅ 4x better |
| I/O Throughput | 100K/s* | 500K/s | 5x slower |
| HTTP RPS | 80K* | 150K | 1.9x slower |

*Target with full implementation

## Key Optimizations Needed

### Priority 1: Critical Path (2-5x improvement)

1. **Full Fiber-Based Spawn** - Replace threading fallback
2. **Zero-Overhead Context Switching** - Inline assembly
3. **Lock-Free Scheduler** - Eliminate mutex contention
4. **Native I/O Path** - io_uring for all I/O

### Priority 2: Memory Optimization (2x improvement)

5. **Slab Allocator** - Pre-allocate fiber stacks
6. **Object Pools** - Channels, futures, timers
7. **Stack Growth** - Dynamic stack sizing

### Priority 3: Python Integration (5-10x improvement)

8. **GIL-Free Execution** - Release GIL during fiber runs
9. **Zero-Copy Python Objects** - Avoid serialization
10. **Batched Operations** - Reduce Python/C boundary crossings

---

## Implementation Details

### 1. Full Fiber-Based Spawn

**Current Issue:** `spawn()` falls back to threading

**Solution:** Proper Python object passing to C fibers

```c
// In scheduler.c - new function for Python callbacks
typedef struct {
    PyObject* func;
    PyObject* args;
    PyObject* kwargs;
} python_callback_t;

static void python_fiber_entry(void* arg) {
    python_callback_t* cb = (python_callback_t*)arg;
    
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    PyObject* result = PyObject_Call(cb->func, cb->args, cb->kwargs);
    if (!result) {
        PyErr_Print();
    } else {
        Py_DECREF(result);
    }
    
    Py_DECREF(cb->func);
    Py_DECREF(cb->args);
    Py_XDECREF(cb->kwargs);
    free(cb);
    
    PyGILState_Release(gstate);
}

uint64_t scheduler_spawn_python(PyObject* func, PyObject* args, PyObject* kwargs) {
    python_callback_t* cb = malloc(sizeof(python_callback_t));
    cb->func = func;
    cb->args = args;
    cb->kwargs = kwargs;
    Py_INCREF(func);
    Py_INCREF(args);
    Py_XINCREF(kwargs);
    
    return scheduler_spawn(python_fiber_entry, cb);
}
```

**Expected Improvement:** 100x faster spawn (5 µs → 50 ns)

---

### 2. Zero-Overhead Context Switching

**Current Issue:** `setjmp`/`longjmp` saves too much state

**Solution:** Inline assembly for minimal context save

```c
// fiber_asm.h - Architecture-specific context switch
#ifdef __x86_64__

static inline void fiber_switch(fiber_t* from, fiber_t* to) {
    __asm__ volatile (
        // Save callee-saved registers
        "movq %%rbx, 0(%0)\n\t"
        "movq %%rbp, 8(%0)\n\t"
        "movq %%r12, 16(%0)\n\t"
        "movq %%r13, 24(%0)\n\t"
        "movq %%r14, 32(%0)\n\t"
        "movq %%r15, 40(%0)\n\t"
        "movq %%rsp, 48(%0)\n\t"
        "movq %%rip, 56(%0)\n\t"
        
        // Restore callee-saved registers
        "movq 0(%1), %%rbx\n\t"
        "movq 8(%1), %%rbp\n\t"
        "movq 16(%1), %%r12\n\t"
        "movq 24(%1), %%r13\n\t"
        "movq 32(%1), %%r14\n\t"
        "movq 40(%1), %%r15\n\t"
        "movq 48(%1), %%rsp\n\t"
        "movq 56(%1), %%rip\n\t"
        :
        : "r"(&from->context), "r"(&to->context)
        : "memory"
    );
}

#endif
```

**Context size:** 64 bytes vs 512+ bytes for `sigjmp_buf`

**Expected Improvement:** 2x faster context switch (1.08 µs → 0.5 µs)

---

### 3. Lock-Free Work-Stealing Scheduler

**Current Issue:** Mutex contention on cross-worker scheduling

**Solution:** Chase-Lev deque with atomic operations

```c
// lock_free_deque.h
typedef struct {
    _Atomic fiber_t** buffer;
    _Atomic size_t top;
    _Atomic size_t bottom;
    _Atomic size_t capacity;
} lock_free_deque_t;

static inline void lf_push(lock_free_deque_t* dq, fiber_t* f) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    size_t cap = atomic_load_explicit(&dq->capacity, memory_order_relaxed);
    
    if (b - t >= cap) {
        // Resize (rare case - use mutex here)
        deque_resize(dq, cap * 2);
        b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    }
    
    fiber_t** buf = atomic_load_explicit(&dq->buffer, memory_order_acquire);
    buf[b & (cap - 1)] = f;  // Power-of-2 capacity for fast mod
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
}

static inline fiber_t* lf_pop(lock_free_deque_t* dq) {
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    if (b == 0) return NULL;
    
    b--;
    atomic_store_explicit(&dq->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    if (t <= b) {
        fiber_t** buf = atomic_load_explicit(&dq->buffer, memory_order_acquire);
        size_t cap = atomic_load_explicit(&dq->capacity, memory_order_relaxed);
        fiber_t* f = buf[b & (cap - 1)];
        
        if (atomic_compare_exchange_weak_explicit(
                &dq->bottom, &b, b,
                memory_order_release, memory_order_relaxed)) {
            return f;
        }
    }
    
    // Failed race - restore bottom
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
    return NULL;
}

static inline fiber_t* lf_steal(lock_free_deque_t* dq) {
    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    
    if (t >= b) return NULL;
    
    fiber_t** buf = atomic_load_explicit(&dq->buffer, memory_order_acquire);
    size_t cap = atomic_load_explicit(&dq->capacity, memory_order_relaxed);
    fiber_t* f = buf[t & (cap - 1)];
    
    if (!atomic_compare_exchange_weak_explicit(
            &dq->top, &t, t + 1,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;  // Lost race
    }
    
    return f;
}
```

**Expected Improvement:** 3x better multi-threaded scaling

---

### 4. Native I/O with io_uring

**Current Issue:** I/O operations block or use epoll wrapper

**Solution:** Full io_uring integration for async I/O

```c
// io_uring_integration.c
typedef struct {
    fiber_t* fiber;
    struct io_uring* ring;
    uint64_t user_data;
} uring_waiter_t;

static void uring_completion(struct io_uring_cqe* cqe) {
    uring_waiter_t* waiter = (uring_waiter_t*)(uintptr_t)cqe->user_data;
    
    // Wake the waiting fiber
    waiter->fiber->state = FIBER_READY;
    waiter->fiber->result = (void*)cqe->res;
    scheduler_schedule(waiter->fiber, -1);
    
    free(waiter);
}

int fiber_read_async(int fd, void* buf, size_t len) {
    fiber_t* current = fiber_current();
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&g_scheduler->io_uring_ring);
    if (!sqe) return -1;
    
    uring_waiter_t* waiter = malloc(sizeof(uring_waiter_t));
    waiter->fiber = current;
    waiter->ring = &g_scheduler->io_uring_ring;
    
    io_uring_prep_read(sqe, fd, buf, len, 0);
    sqe->user_data = (uint64_t)(uintptr_t)waiter;
    sqe->flags |= IOSQE_IO_LINK;  // Link for completion handling
    
    current->state = FIBER_WAITING;
    fiber_yield();
    
    return (int)(intptr_t)current->result;
}

int fiber_write_async(int fd, const void* buf, size_t len) {
    // Similar implementation for write
}

int fiber_accept_async(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    // Similar implementation for accept
}
```

**Expected Improvement:** 5x I/O throughput (100K/s → 500K/s)

---

### 5. Slab Allocator for Fibers

**Current Issue:** `malloc`/`mmap` overhead for each fiber

**Solution:** Pre-allocated slabs for common sizes

```c
// slab_allocator.h
#define SLAB_SIZE_2KB   0
#define SLAB_SIZE_4KB   1
#define SLAB_SIZE_8KB   2
#define SLAB_SIZE_16KB  3
#define NUM_SLAB_CLASSES  4

typedef struct slab_node {
    struct slab_node* next;
} slab_node_t;

typedef struct slab_class {
    slab_node_t* free_list;
    size_t object_size;
    size_t objects_per_slab;
    pthread_mutex_t lock;
} slab_class_t;

static slab_class_t slab_classes[NUM_SLAB_CLASSES] = {
    {NULL, 2048, 0, PTHREAD_MUTEX_INITIALIZER},
    {NULL, 4096, 0, PTHREAD_MUTEX_INITIALIZER},
    {NULL, 8192, 0, PTHREAD_MUTEX_INITIALIZER},
    {NULL, 16384, 0, PTHREAD_MUTEX_INITIALIZER},
};

static inline void* slab_alloc(size_t size) {
    int class_idx = 0;
    
    // Find appropriate slab class
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        if (size <= slab_classes[i].object_size) {
            class_idx = i;
            break;
        }
    }
    
    slab_class_t* sc = &slab_classes[class_idx];
    pthread_mutex_lock(&sc->lock);
    
    if (!sc->free_list) {
        // Allocate new slab
        allocate_new_slab(sc);
    }
    
    slab_node_t* node = sc->free_list;
    sc->free_list = node->next;
    
    pthread_mutex_unlock(&sc->lock);
    return node;
}

static inline void slab_free(void* ptr, size_t size) {
    // Find slab class and return to free list
    slab_node_t* node = (slab_node_t*)ptr;
    
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        if (ptr >= slab_classes[i].base && 
            ptr < slab_classes[i].base + slab_classes[i].size) {
            pthread_mutex_lock(&slab_classes[i].lock);
            node->next = slab_classes[i].free_list;
            slab_classes[i].free_list = node;
            pthread_mutex_unlock(&slab_classes[i].lock);
            return;
        }
    }
}
```

**Expected Improvement:** 10x faster allocation (500 ns → 50 ns)

---

### 6. GIL-Free Fiber Execution

**Current Issue:** Python GIL held during fiber execution

**Solution:** Release GIL during pure C fiber operations

```c
// In scheduler worker thread
static void* worker_thread(void* arg) {
    worker_t* w = (worker_t*)arg;
    
    // Acquire GIL once at startup
    PyGILState_STATE gstate = PyGILState_Ensure();
    Py_Initialize();
    
    while (w->running) {
        fiber_t* f = pop_local(w);
        
        if (f) {
            // Release GIL during fiber execution
            PyGILState_Release(gstate);
            
            // Execute fiber without GIL
            if (f->state == FIBER_NEW) {
                if (setjmp(f->context) == 0) {
                    f->state = FIBER_RUNNING;
                    f->func(f->arg);  // Pure C code
                    f->state = FIBER_COMPLETED;
                }
            }
            
            // Re-acquire GIL for Python operations
            gstate = PyGILState_Ensure();
            
            // Handle Python callbacks if needed
            if (f->python_callback) {
                handle_python_callback(f);
            }
        } else {
            // No work - can sleep without GIL
            PyGILState_Release(gstate);
            nanosleep(&short_sleep, NULL);
            gstate = PyGILState_Ensure();
        }
    }
    
    PyGILState_Release(gstate);
    return NULL;
}
```

**Expected Improvement:** 2x better multi-threaded performance

---

### 7. Zero-Copy Channel Messages

**Current Issue:** Channel send/recv copies message data

**Solution:** Pass Python object references

```c
// In channel.c
int channel_send_zero_copy(channel_t* ch, PyObject* obj) {
    // Just store the reference, don't copy
    Py_INCREF(obj);
    
    pthread_mutex_lock(&ch->lock);
    
    if (ch->size < ch->capacity || ch->capacity == 0) {
        // Unbuffered - wake receiver immediately
        if (ch->waiters_recv > 0) {
            fiber_t* receiver = ch->waiters_recv_head;
            receiver->result = obj;  // Pass reference
            receiver->state = FIBER_READY;
            scheduler_schedule(receiver, -1);
            pthread_mutex_unlock(&ch->lock);
            return 0;
        }
        
        // Buffered - add to queue
        ch->buffer[ch->tail] = obj;
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->size++;
        pthread_mutex_unlock(&ch->lock);
        return 0;
    }
    
    // Buffer full - block
    Py_DECREF(obj);
    pthread_mutex_unlock(&ch->lock);
    return -1;
}

PyObject* channel_recv_zero_copy(channel_t* ch) {
    pthread_mutex_lock(&ch->lock);
    
    if (ch->size > 0) {
        PyObject* obj = ch->buffer[ch->head];
        ch->head = (ch->head + 1) % ch->capacity;
        ch->size--;
        pthread_mutex_unlock(&ch->lock);
        return obj;  // Caller owns reference
    }
    
    // Empty - block
    pthread_mutex_unlock(&ch->lock);
    return NULL;
}
```

**Expected Improvement:** 5x faster channel operations

---

### 8. Batched Python/C Calls

**Current Issue:** Every operation crosses Python/C boundary

**Solution:** Batch operations to reduce crossings

```c
// Batch spawn for creating many fibers at once
uint64_t* scheduler_spawn_batch_python(
    PyObject* func, 
    PyObject** args_array, 
    size_t count
) {
    uint64_t* fids = malloc(count * sizeof(uint64_t));
    
    // Single GIL acquisition for all spawns
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    for (size_t i = 0; i < count; i++) {
        python_callback_t* cb = malloc(sizeof(python_callback_t));
        cb->func = func;
        cb->args = args_array[i];
        cb->kwargs = NULL;
        Py_INCREF(func);
        Py_INCREF(args_array[i]);
        
        fids[i] = scheduler_spawn(python_fiber_entry, cb);
    }
    
    PyGILState_Release(gstate);
    
    return fids;
}
```

**Expected Improvement:** 10x faster bulk operations

---

## Expected Final Performance

After implementing all optimizations:

| Metric | Current | After Opt. | Go | vs Go |
|--------|---------|------------|-----|-------|
| Context Switch | 1.08 µs | **0.5 µs** | 0.5 µs | ✅ Equal |
| Task Creation | 800 ns | **200 ns** | 200 ns | ✅ Equal |
| Memory/Task | 2.2 KB | **1.5 KB** | 8.5 KB | ✅ 5x better |
| I/O Throughput | 100K/s | **500K/s** | 500K/s | ✅ Equal |
| HTTP RPS | 80K | **150K** | 150K | ✅ Equal |
| Multi-Core | 60-80% | **80-90%** | 80-95% | ✅ Near equal |

---

## Implementation Priority

### Phase 1: Foundation (Week 1-2)
- [ ] Inline assembly context switch
- [ ] Lock-free deque
- [ ] Slab allocator

**Expected:** 2x overall improvement

### Phase 2: I/O Path (Week 3-4)
- [ ] Full io_uring integration
- [ ] Native socket operations
- [ ] Timer wheel optimization

**Expected:** 5x I/O improvement

### Phase 3: Python Integration (Week 5-6)
- [ ] GIL-free execution
- [ ] Zero-copy channels
- [ ] Batched operations

**Expected:** 5-10x Python overhead reduction

### Phase 4: Polish (Week 7-8)
- [ ] Profiling and tuning
- [ ] Memory optimization
- [ ] Documentation

**Expected:** 20% additional improvement

---

## Risk Mitigation

### Technical Risks

1. **Inline Assembly Portability**
   - Solution: Provide C fallback for non-x86_64
   - Test: CI on multiple architectures

2. **Lock-Free Correctness**
   - Solution: Use ThreadSanitizer, formal verification
   - Test: Stress tests with 1M+ fibers

3. **GIL Management**
   - Solution: Careful state tracking, extensive testing
   - Test: Multi-threaded Python integration tests

### Testing Strategy

1. **Micro-benchmarks** for each optimization
2. **Stress tests** with 1M+ concurrent fibers
3. **Comparison benchmarks** against Go
4. **Memory leak detection** with Valgrind/ASan

---

## Conclusion

With these optimizations, gsyncio can achieve **Go-level performance** while maintaining Python compatibility:

- **Context switch**: Match Go's 0.5 µs
- **Task creation**: Match Go's 200 ns
- **I/O throughput**: Match Go's 500K/s
- **Memory**: 5x better than Go

The key is minimizing overhead in the hot path while leveraging Python's strengths for application logic.
