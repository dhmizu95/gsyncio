# No-GIL Performance Improvement Plan

## Problem Statement

**Current Performance Issue:**
- Individual spawn: **34K/s** (gsyncio pure Python)
- asyncio spawn: **299K/s** 
- **Performance gap: 8.8x slower**

**Root Cause:** Python GIL overhead in pure Python fallback implementation

## Analysis

### Current Spawn Implementation (Pure Python)

```python
def spawn(func, *args):
    """Pure Python spawn using threading"""
    t = threading.Thread(target=func, args=args)
    t.start()
    return t
```

**Problems:**
1. **Thread creation overhead**: Each spawn creates a new OS thread
2. **GIL contention**: All threads compete for the GIL
3. **No fiber pooling**: No reuse of execution contexts
4. **No M:N scheduling**: Maps 1:1 to OS threads instead of M:N

### Why asyncio is Faster

asyncio doesn't create threads - it uses:
- **Coroutine objects** (lightweight, ~1KB)
- **Single-threaded event loop** (no GIL contention)
- **Generator-based yielding** (no context switch overhead)

```python
# asyncio creates coroutine objects, not threads
tasks = [asyncio.create_task(coro()) for _ in range(1000)]
```

## Solutions

### Option 1: Compile C Extension (Immediate Fix) ⭐ RECOMMENDED

**Action:** Ensure Cython extension is compiled and used

**Expected Improvement:** 10-50x faster spawn

**Steps:**
1. Install Cython: `pip install cython`
2. Build extension: `python setup.py build_ext --inplace`
3. Verify: Check `gsyncio._HAS_CYTHON == True`

**Why this works:**
- C extension uses fiber pooling (pre-allocated fibers)
- Direct `setjmp`/`longjmp` context switching (<1µs)
- No Python object allocation per spawn
- Work-stealing scheduler distributes load

**Benchmark Target:**
- Spawn rate: **500K/s - 1M/s** (with C extension)
- Context switch: **<1µs**

### Option 2: Free-Threaded Python (No-GIL Python) 🚀 LONG-TERM

**Action:** Support Python 3.13+ free-threaded build (PEP 703)

**Expected Improvement:** True parallelism, no GIL contention

**Steps:**
1. Test with Python 3.13 free-threaded build
2. Remove GIL-dependent code (threading locks)
3. Use atomic operations for shared state
4. Update C extension for no-GIL mode

**Challenges:**
- Python 3.13+ only (limited adoption)
- Requires careful audit of thread safety
- Some C APIs still GIL-dependent

**Code Changes Needed:**
```c
// In C extension, release GIL during long operations
Py_BEGIN_ALLOW_THREADS
// ... long-running C code ...
Py_END_ALLOW_THREADS
```

### Option 3: Hybrid Approach (Best of Both) ⭐ RECOMMENDED

**Action:** Use asyncio for simple spawns, fibers for heavy workloads

**Implementation:**
```python
def task(func, *args, **kwargs):
    if _HAS_CYTHON:
        # Use fast C fiber spawn
        return _spawn(func, *args)
    else:
        # Fallback: batch into asyncio tasks
        import asyncio
        loop = asyncio.get_event_loop()
        return loop.create_task(asyncio.to_thread(func, *args))
```

**Benefits:**
- Best performance when C extension available
- Graceful fallback to asyncio
- No breaking changes to API

### Option 4: Optimize Pure Python Fallback

**Action:** Reduce overhead in pure Python mode

**Improvements:**

#### 4.1 Thread Pooling
```python
# Pre-create thread pool instead of spawning new threads
_thread_pool = []
_thread_pool_size = 0

def spawn(func, *args):
    global _thread_pool, _thread_pool_size
    if _thread_pool_size < MAX_THREADS:
        t = threading.Thread(target=_worker_loop, daemon=True)
        t.start()
        _thread_pool.append(t)
        _thread_pool_size += 1
    # Queue work to existing thread
    _work_queue.put((func, args))
```

**Expected:** 2-3x improvement (100K/s)

#### 4.2 Batch Spawning
```python
def task(func, *args):
    # Accumulate tasks and spawn in batches
    _pending_tasks.append((func, args))
    if len(_pending_tasks) >= BATCH_SIZE:
        _spawn_batch(_pending_tasks)
        _pending_tasks.clear()
```

**Expected:** 5-10x improvement for batched spawns

#### 4.3 Object Pooling
```python
# Pool reusable task wrappers
_task_wrapper_pool = []

def _get_task_wrapper(func, args):
    if _task_wrapper_pool:
        wrapper = _task_wrapper_pool.pop()
        wrapper.func, wrapper.args = func, args
        return wrapper
    return _TaskWrapper(func, args)
```

**Expected:** 1.5-2x improvement

## Performance Targets

| Metric | Current (Pure Python) | Target (C Extension) | Target (No-GIL) |
|--------|----------------------|---------------------|-----------------|
| Spawn Rate | 34K/s | 500K/s - 1M/s | 1M+/s |
| Context Switch | N/A | <1µs | <0.5µs |
| Memory/Task | ~50KB | 2-32KB | 2-8KB |
| Max Concurrency | 10K | 1M+ | 10M+ |

## Implementation Priority

### Phase 1: Immediate (Week 1)
1. ✅ **Document C extension build process**
2. ✅ **Add build verification in tests**
3. ✅ **Improve error messages when C extension missing**

### Phase 2: Short-term (Month 1)
1. 🔄 **Optimize pure Python fallback**
   - Thread pooling
   - Batch spawning
   - Object pooling
2. 🔄 **Add performance regression tests**
3. 🔄 **Benchmark suite comparing vs asyncio**

### Phase 3: Medium-term (Quarter 1)
1. 📋 **Support Python 3.13+ free-threaded build**
2. 📋 **Audit C extension for GIL assumptions**
3. 📋 **Add atomic operations for shared state**

### Phase 4: Long-term (Year 1)
1. 📋 **Full no-GIL native implementation**
2. 📋 **Assembly-optimized context switching**
3. 📋 **True M:N:P scheduler (like Go)**

## Benchmark Comparison

### Current State (Pure Python)
```
Task Spawn (1000 tasks):
  gsyncio:  29.4ms (34K/s)
  asyncio:   3.3ms (303K/s)
  Ratio:     8.9x slower
```

### Target State (C Extension Compiled)
```
Task Spawn (1000 tasks):
  gsyncio:   1.0ms (1M/s)
  asyncio:   3.3ms (303K/s)
  Ratio:     3.3x faster
```

### Ultimate Target (No-GIL + Optimized)
```
Task Spawn (1000 tasks):
  gsyncio:   0.4ms (2.5M/s)
  asyncio:   3.3ms (303K/s)
  Go:        0.4ms (2.5M/s)
  Ratio:     Comparable to Go
```

## Recommendations

### For Users Now:
1. **Build C extension:** `pip install -e ".[dev]"`
2. **Verify compilation:** Check `gsyncio._HAS_CYTHON`
3. **Use batch spawning:** `task_batch()` for bulk operations

### For Developers:
1. **Prioritize C extension compilation** in CI/CD
2. **Add performance gates** (fail if >2x slower than baseline)
3. **Test with Python 3.13+** free-threaded builds

### For Project Direction:
1. **Short-term:** Optimize pure Python fallback
2. **Medium-term:** Support no-GIL Python
3. **Long-term:** Match Go coroutine performance

## References

- [PEP 703: Making the Global Interpreter Lock Optional](https://peps.python.org/pep-0703/)
- [Python 3.13 Free-Threaded Build](https://docs.python.org/3.13/whatsnew/3.13.html)
- [Go M:N Scheduler](https://go.dev/doc/scheduler)
- [libuv Thread Pool](https://docs.libuv.org/en/v1.x/threadpool.html)

## Conclusion

The **8.8x performance gap** is primarily due to running in pure Python mode without the C extension compiled. The C extension provides:
- Fiber pooling (no per-task allocation)
- Direct context switching (setjmp/longjmp)
- M:N scheduling (not 1:1 threading)

**Immediate action:** Ensure C extension is compiled for production use.

**Long-term:** Support no-GIL Python for true parallelism matching Go performance.
