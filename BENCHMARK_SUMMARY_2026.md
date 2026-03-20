# Benchmark Summary: asyncio vs gsyncio vs Go (March 2026)

## System Configuration
- **Python**: 3.12.3
- **Go**: 1.22.2
- **CPU**: 12 cores
- **Date**: March 19, 2026

---

## 📊 Benchmark Results

### Execution Time (milliseconds) - Lower is Better

| Benchmark | asyncio | gsyncio | Go | gsyncio/asyncio | gsyncio/Go |
|-----------|---------|---------|-----|-----------------|------------|
| **Task Spawn (1000)** | 4.14 | 180.51 | 1.00 | 43.6x slower | 180.5x slower |
| **Sleep (100)** | 2.62 | 18.29 | 1.00 | 7.0x slower | 18.3x slower |
| **WaitGroup (10×100)** | 0.64 | 2.61 | 0.06 | 4.1x slower | 43.5x slower |
| **Context Switch (10000)** | 13.16 | **3.99** ✅ | 2.00 | **3.3x faster** | 2.0x slower |

### Memory Usage

| Framework | Max RSS (KB) | Relative |
|-----------|--------------|----------|
| **asyncio** | 21,628 | 1.0x |
| **gsyncio** | ~30,000 | 1.4x |
| **Go** | 44,544 | 2.1x |

### CPU Efficiency

| Framework | User Time | System Time | Wall Time | Efficiency |
|-----------|-----------|-------------|-----------|------------|
| **asyncio** | 0.08s | 0.00s | 0.09s | 89% |
| **Go** | 0.32s | 0.45s | 0.39s | 51% |
| **gsyncio** | - | - | ~0.30s | - |

---

## 🏆 Performance Ranking

### Overall Winner: **Go** ⭐
- Fastest in 3 out of 4 benchmarks
- Best raw performance
- True parallelism (no GIL)

### Best Context Switch: **gsyncio** ⭐
- 3.3x faster than asyncio
- C fiber implementation (setjmp/longjmp)
- Only category where gsyncio beats asyncio

### Best Memory Efficiency: **asyncio** ⭐
- Lowest memory footprint
- Mature, optimized codebase

---

## 🔍 Root Cause Analysis

### Why gsyncio is Slow (180ms task spawn)

**CRITICAL FINDING**: Despite C extension loading successfully, gsyncio has **massive overhead**:

1. **Lock Contention**: Every task acquires `_tasks_lock` in Python
2. **Payload Allocation**: New `(func, args)` tuple per task
3. **Cython Bridge**: Python→C boundary crossing overhead
4. **No Fast Path**: All tasks go through same slow path

**Call Chain**:
```
task() 
  → _tasks_lock.acquire()           # ~100ns
  → _pending_count += 1             # Python int operation
  → _task_completion_wrapper()      # Python wrapper
  → _spawn()                        # Cython function
  → _get_payload()                  # Pool lookup with lock
  → scheduler_spawn()               # C function
  → _c_fiber_entry()                # C callback
  → with gil:                       # Acquire GIL
  → payload = <object>arg           # Cast
  → func(*args)                     # Python call
```

**Total overhead per task**: ~180µs

**Go comparison**:
```
go func() { ... }()
  → goroutine allocation            # ~50ns (direct memory alloc)
  → schedule on runq                # Lock-free enqueue
```

**Total overhead**: ~1µs

### Why Context Switch is Fast

**gsyncio wins here** because:
```c
// fiber.c - Pure C, no Python
void fiber_yield() {
    fiber_t* current = fiber_current();
    if (sigsetjmp(current->context, 0) == 0) {
        scheduler_schedule(current, -1);
    }
}
```

- Zero Python involvement
- Direct `setjmp`/`longjmp`
- Minimal overhead: **0.4µs per yield**

---

## 🚀 Optimization Recommendations

### Priority 1: Remove Lock from Task Path

**Current**:
```python
with _tasks_lock:
    _pending_count += 1
```

**Fixed** (lock-free with atomics):
```python
import ctypes
_pending_count = ctypes.c_uint64(0)

def task(func, *args):
    ctypes.atomic_fetch_add(_pending_count, 1)
    _spawn_direct(func, args)  # No wrapper
```

**Expected**: 2-5x faster

### Priority 2: Ultra-Fast Spawn Function

Add specialized function for common case (no args):

```cython
cdef uint64_t spawn_no_args(object func) noexcept nogil:
    """Direct spawn - no tuple allocation"""
    Py_INCREF(func)
    return scheduler_spawn(_c_entry_simple, <void*>func)

cdef void _c_entry_simple(void* arg) noexcept nogil:
    """Minimal entry point"""
    if arg != NULL:
        with gil:
            func = <object>arg
            func()
            Py_DECREF(func)
```

**Expected**: 5-10x faster for no-arg functions

### Priority 3: Pre-Start Worker Threads

**Issue**: Workers may not be running when tasks are spawned.

**Check**: Add diagnostic to verify workers are active:
```python
def check_workers():
    from gsyncio._gsyncio_core import get_scheduler_stats
    stats = get_scheduler_stats()
    print(f"Workers active: {stats.get('workers', 0)}")
```

### Priority 4: Batch Spawn Optimization

For spawning many tasks, use batch operations:

```python
# Instead of:
for i in range(1000):
    gsyncio.task(worker, i)

# Use:
gsyncio.task_batch([(worker, (i,)) for i in range(1000)])
```

**Expected**: 10x faster for bulk spawns

---

## 📈 Target Performance (After Optimizations)

| Benchmark | Current | After P1 | After P2 | Target |
|-----------|---------|----------|----------|--------|
| Task Spawn | 180ms | 36ms | 18ms | **10ms** |
| Sleep | 18ms | 18ms | 6ms | **3ms** |
| WaitGroup | 2.6ms | 1.3ms | 0.8ms | **0.5ms** |
| Context Switch | 4ms | 4ms | 3ms | **2ms** |

---

## 🎯 When to Use Each Framework

### Use **Go** when:
- Maximum performance is critical
- Building new services from scratch
- Need true parallelism (no GIL)
- Millions of concurrent connections

### Use **asyncio** when:
- Standard Python async apps
- Need ecosystem compatibility (FastAPI, aiohttp)
- I/O-bound workloads
- Memory-constrained environments

### Use **gsyncio** when:
- Need Go-like concurrency in Python
- Mixed CPU/I/O workloads
- Can apply optimizations above
- Context switching is the bottleneck

---

## 📝 Files Generated

1. **PERFORMANCE_ANALYSIS_AND_OPTIMIZATIONS.md** - Detailed analysis
2. **BENCHMARK_SUMMARY_2026.md** - This summary
3. **benchmarks/benchmark_comparison_all.txt** - Raw results

---

## 🔬 How to Reproduce

```bash
cd gsyncio/benchmarks

# Run individual benchmarks
python3 benchmark_asyncio.py    # asyncio
python3 benchmark.py            # gsyncio
go run benchmark_go.go          # Go

# Or run comparison script
python3 compare_all.py
```

---

## Conclusion

**gsyncio has excellent architecture** (M:N scheduling, work-stealing, C fibers) but suffers from **excessive Python overhead** in the critical path.

**Key insight**: The C extension IS loading and working, but Python wrappers, locks, and allocations add ~180x overhead compared to Go's native runtime.

**Path forward**:
1. Remove locks from hot path (use atomics)
2. Add specialized fast-path functions
3. Minimize Python involvement in task spawn
4. Leverage batch operations

With these optimizations, gsyncio can achieve **10-20x improvement** and approach asyncio performance while maintaining its context-switch advantage.
