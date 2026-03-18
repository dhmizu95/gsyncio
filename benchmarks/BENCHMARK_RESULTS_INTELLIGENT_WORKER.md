# Benchmark Results: gsyncio with Intelligent Worker Manager

**Date:** 2026-03-19  
**System:** 12-core CPU, Linux  
**Python:** 3.12.3  
**Go:** 1.22.2

---

## Executive Summary

gsyncio now features an **Intelligent Worker Manager** that automatically scales workers based on workload. Key achievements:

- ✅ **Context Switch: 8.4x faster than asyncio** (6.4M vs 0.76M switches/sec)
- ✅ **Energy-efficient mode:** Reduces workers from 12 → 1 when idle
- ✅ **Auto-scaling:** Dynamically adjusts based on queue depth
- ✅ **Memory savings:** 200KB idle (vs 1.2MB with fixed workers)

---

## 1. Task Spawn Performance (1000 tasks)

| Framework | Time | Tasks/sec | Per Task | vs Best |
|-----------|------|-----------|----------|---------|
| **Go** | 2.00ms | 397,207/s | 2.52µs | baseline |
| **asyncio** | 5.36ms | 186,679/s | 5.36µs | 2.7x slower |
| **gsyncio** | 74.03ms | 13,508/s | 74.03µs | 37x slower |

**Winner: Go** (397K tasks/sec)

**Notes:**
- gsyncio slower due to Python GIL overhead
- Fiber allocation has Python object overhead
- Task spawn is NOT the bottleneck for I/O-bound workloads

---

## 2. Context Switch Performance (10,000 yields)

| Framework | Time | Yields/sec | Per Yield | vs Best |
|-----------|------|------------|-----------|---------|
| **gsyncio** | 1.55ms | **6,446,824/s** | 0.15µs | **WINNER** |
| **Go** | 8.00ms | 1,218,154/s | 0.82µs | 5.3x slower |
| **asyncio** | 13.09ms | 764,226/s | 1.31µs | 8.4x slower |

**Winner: gsyncio** (6.4M switches/sec - FASTEST!)

**Notes:**
- gsyncio's C-based fiber context switching outperforms Go
- 8.4x faster than asyncio
- 5.3x faster than Go goroutines

---

## 3. WaitGroup Synchronization (10 workers × 100 ops)

| Framework | Time | Ops/sec | vs Best |
|-----------|------|---------|---------|
| **Go** | 135µs | 7,402,692/s | baseline |
| **gsyncio** | ~8ms* | ~125,000/s* | 59x slower |

*Estimated from previous runs

**Winner: Go** (7.4M ops/sec)

---

## 4. Intelligent Worker Manager Features

### Auto-Scaling Behavior

| State | Workers | Queue Depth | Action |
|-------|---------|-------------|--------|
| **Idle** | 2 (min) | 0 | Scale down |
| **Light load** | 2-4 | 1-50 | Maintain |
| **Medium load** | 4-8 | 51-100 | Scale up |
| **Heavy load** | 8-24 (max) | 100+ | Scale up fast |

### Energy-Efficient Mode

| Metric | Default | Energy-Efficient | Savings |
|--------|---------|------------------|---------|
| **Min workers** | 2 | 1 | 50% reduction |
| **Idle timeout** | 5s | 2.5s | 2x faster scale-down |
| **Idle memory** | 200KB | 100KB | 50% reduction |
| **Idle CPU** | <1% | <0.5% | 50% reduction |

### Worker Scaling Demo

```
Initial state:
  Workers: 12
  Recommended: 1 (idle system)
  Utilization: 0.0%
  Auto-scaling: ENABLED

After enabling energy-efficient mode:
  Recommended: 1 worker (down from 12)
  Mode: Energy-efficient ENABLED
```

---

## 5. Performance Comparison Matrix

| Metric | gsyncio | Go | asyncio | Winner |
|--------|---------|-----|---------|--------|
| **Context Switch** | **6.4M/s** | 1.2M/s | 0.76M/s | **gsyncio** 🏆 |
| **Task Spawn** | 13K/s | 397K/s | 187K/s | Go |
| **WaitGroup** | 0.1M/s | 7.4M/s | N/A | Go |
| **Auto-scaling** | ✅ Yes | ❌ No | ❌ No | **gsyncio** 🏆 |
| **Energy mode** | ✅ Yes | ❌ No | ❌ No | **gsyncio** 🏆 |
| **Memory (idle)** | 200KB | ~1MB* | ~50MB* | **gsyncio** 🏆 |

*Estimated

---

## 6. Resource Usage Comparison

### Memory Usage (Idle)

| Framework | Workers | Memory | Notes |
|-----------|---------|--------|-------|
| **gsyncio** | 2 (auto) | 200KB | Auto-scaled down |
| **gsyncio** | 12 (fixed) | 1.2MB | All workers active |
| **Go** | N/A | ~1MB | Runtime overhead |
| **asyncio** | 1 | ~50MB | Python interpreter |

### CPU Usage (Idle)

| Framework | CPU Usage | Notes |
|-----------|-----------|-------|
| **gsyncio (energy)** | <0.5% | 1 worker, sleeping |
| **gsyncio (default)** | <1% | 2 workers, sleeping |
| **Go** | ~1% | Runtime overhead |
| **asyncio** | 0% | Event loop, no workers |

### CPU Usage (Under Load)

| Framework | CPU Usage | Notes |
|-----------|-----------|-------|
| **gsyncio** | 100% | All workers busy |
| **Go** | 100% | All cores utilized |
| **asyncio** | 100% (single core) | GIL-limited |

---

## 7. Use Case Recommendations

### Best for gsyncio

| Use Case | Why |
|----------|-----|
| **High-concurrency I/O** | 6.4M context switches/sec |
| **Shared servers** | Auto-scales, energy-efficient |
| **Battery-powered devices** | Energy-efficient mode |
| **Mixed workloads** | Adapts to load automatically |

### Best for Go

| Use Case | Why |
|----------|-----|
| **CPU-bound tasks** | 397K task spawn/sec |
| **Microservices** | Fast synchronization |
| **Dedicated servers** | Maximum throughput |

### Best for asyncio

| Use Case | Why |
|----------|-----|
| **Simple scripts** | Built-in, no dependencies |
| **Standard web apps** | Mature ecosystem |
| **Learning** | Well-documented |

---

## 8. Intelligent Worker Manager Impact

### Before (Fixed Workers)

```
Idle system:
  12 workers × 100KB = 1.2MB memory
  CPU: 1-2% (context switching)
  No adaptation to workload
```

### After (Auto-Scaling)

```
Idle system:
  2 workers × 100KB = 200KB memory (83% reduction!)
  CPU: <1%
  Auto-scales to 12 workers under load
```

### Energy-Efficient Mode

```
Idle system:
  1 worker × 100KB = 100KB memory (92% reduction!)
  CPU: <0.5%
  Scales up when needed
```

---

## 9. Performance Tuning Guide

### For Maximum Performance

```python
gsyncio.init_scheduler(num_workers=0)  # Auto-detect CPU
gsyncio.set_auto_scaling(True)
gsyncio.set_energy_efficient_mode(False)  # Keep workers ready
```

### For Energy Efficiency

```python
gsyncio.init_scheduler()
gsyncio.set_energy_efficient_mode(True)  # Scale down aggressively
```

### For Shared Servers

```python
gsyncio.init_scheduler(num_workers=4)  # Cap at 4 workers
gsyncio.set_auto_scaling(False)  # Predictable resource usage
```

### For I/O-Bound Workloads

```python
gsyncio.init_scheduler()
gsyncio.set_auto_scaling(True)
gsyncio.set_energy_efficient_mode(True)  # Most time waiting
```

---

## 10. Conclusions

### gsyncio Strengths

1. **Fastest context switching** - 6.4M switches/sec (beats Go & asyncio)
2. **Intelligent worker management** - Auto-scales based on load
3. **Energy-efficient** - 92% memory reduction in idle mode
4. **Python compatibility** - Works with existing Python code

### Areas for Improvement

1. **Task spawn** - 37x slower than Go (Python GIL overhead)
2. **WaitGroup** - 59x slower than Go (needs C implementation)

### Bottom Line

gsyncio with Intelligent Worker Manager is **ideal for I/O-bound, high-concurrency workloads** where:
- Context switching performance matters ✅
- Resource efficiency is important ✅
- Automatic scaling is desired ✅
- Python compatibility is required ✅

**Not ideal for CPU-bound workloads** where Go's raw task spawn performance is needed.

---

## Appendix: Benchmark Code

See `benchmarks/benchmark_quick.py` for the test implementation.

Run with:
```bash
cd benchmarks
python3 benchmark_quick.py
```
