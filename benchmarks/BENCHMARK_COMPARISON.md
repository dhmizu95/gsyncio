# gsyncio vs Go Coroutines Performance Comparison

## Overview
This document compares the performance of gsyncio (Python fiber-based concurrency) with Go coroutines (goroutines).

## Benchmark Results

| Benchmark | gsyncio (ms) | Go (ms) | Slowdown |
|-----------|--------------|---------|----------|
| Task Spawn (1000) | 159.08 | 0.40 | 394.7x |
| Sleep (100) | 20.68 | 1.00 | 20.7x |
| WaitGroup (10×100) | 2.12 | 0.03 | 68.4x |
| Context Switch (10000) | 1.55 | 1.00 | 1.6x |

**Geometric Mean Slowdown: 30.5x**

## Key Findings

### 1. Task Spawn Performance
- **Go**: 0.40ms for 1000 goroutines (~0.4µs per goroutine)
- **gsyncio**: 159.08ms for 1000 tasks (~159µs per task)
- **Analysis**: Go's goroutines are extremely lightweight (2KB stack initially) vs Python threads (8MB stack). Go uses M:N scheduling where many goroutines map to fewer OS threads.

### 2. Sleep Performance
- **Go**: 1.00ms for 100 goroutines sleeping 1ms each
- **gsyncio**: 20.68ms for 100 tasks sleeping 1ms each
- **Analysis**: Go's scheduler is highly optimized for sleep operations. gsyncio uses Python's `time.sleep()` which has overhead.

### 3. WaitGroup Synchronization
- **Go**: 0.03ms for 10 workers × 100 operations
- **gsyncio**: 2.12ms for same workload
- **Analysis**: Go's WaitGroup is implemented in C/assembly with minimal overhead. gsyncio uses Python threading primitives.

### 4. Context Switching
- **Go**: 1.00ms for 10000 yields
- **gsyncio**: 1.55ms for 10000 yields
- **Analysis**: gsyncio is actually slightly faster here due to Python's GIL and cooperative scheduling. However, this is a micro-benchmark that doesn't reflect real-world scenarios.

## Architecture Differences

### Go Coroutines
- **Scheduling**: M:N model (many goroutines on fewer OS threads)
- **Stack Management**: Segmented stacks (grows as needed, starting at 2KB)
- **Context Switching**: Implemented in assembly, very fast
- **Memory**: Minimal per-goroutine overhead (~2KB)
- **Blocking**: Non-blocking I/O integrated into runtime

### gsyncio (Python)
- **Scheduling**: 1:1 model (each task is a Python thread)
- **Stack Management**: Fixed OS thread stack (typically 8MB)
- **Context Switching**: OS thread context switches (slower)
- **Memory**: High per-thread overhead (MBs)
- **Blocking**: Uses Python's blocking I/O with GIL limitations

## Conclusion

Go coroutines are significantly faster than gsyncio for most operations due to:
1. **Lightweight goroutines**: 2KB vs 8MB stacks
2. **Optimized runtime**: Custom scheduler written in assembly
3. **Non-blocking I/O**: Integrated into the runtime
4. **Memory efficiency**: Minimal per-goroutine overhead

gsyncio trades performance for Python compatibility. It's suitable for:
- Python applications needing simple concurrency
- Applications where Go ecosystem isn't available
- Cases where Python's rich library ecosystem is required

## Running the Benchmarks

```bash
# From the benchmarks directory
cd benchmarks

# Python gsyncio benchmark
python3 benchmark.py

# Go benchmark
go run benchmark_go.go

# Comparison
python3 compare_benchmarks.py
```

## System Information
- **Python**: 3.12.3
- **Go**: 1.22.2
- **CPU Cores**: 12
- **C Extension**: False (pure Python implementation)
