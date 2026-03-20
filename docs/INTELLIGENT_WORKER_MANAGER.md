# Intelligent Worker Management

gsyncio now includes an **Intelligent Worker Manager** that automatically adjusts the number of worker threads based on workload patterns.

## Features

- **Auto-scaling** - Automatically adds/removes workers based on queue depth
- **Energy-efficient mode** - Reduces workers when idle to save resources
- **Real-time monitoring** - Track worker utilization and load
- **Smart thresholds** - Configurable scale up/down triggers

## Usage

### Basic Usage

```python
import gsyncio

# Initialize with auto-scaling enabled (default)
gsyncio.init_scheduler()

# Enable energy-efficient mode (fewer workers when idle)
gsyncio.set_energy_efficient_mode(True)

# Check recommended workers
print(f"Recommended: {gsyncio.get_recommended_workers()} workers")
```

### Manual Control

```python
# Disable auto-scaling
gsyncio.set_auto_scaling(False)

# Manually set worker count
gsyncio.init_scheduler(num_workers=4)  # Fixed 4 workers
```

### Monitoring

```python
# Get worker utilization (0-100%)
utilization = gsyncio.get_worker_utilization()
print(f"Workers are {utilization:.1f}% utilized")

# Get current recommendation
recommended = gsyncio.get_recommended_workers()
print(f"System recommends {recommended} workers")

# Manually trigger scaling check
gsyncio.check_worker_scaling()
```

## How It Works

### Auto-Scaling Algorithm

The worker manager monitors:
1. **Queue depth** - Number of pending tasks
2. **Idle time** - How long workers have been idle
3. **Work steals** - Load balancing activity

**Scale UP when:**
- Queue depth > 100 tasks (configurable)
- High utilization (>80%)
- Frequent work stealing

**Scale DOWN when:**
- Queue depth < 10 tasks (configurable)
- Idle for >5 seconds (configurable)
- Low utilization (<20%)

### Worker Lifecycle

```
Idle State (min workers)
    ↓ [Queue depth > threshold]
Scaling Up (add workers)
    ↓ [Queue depth < threshold + idle timeout]
Scaling Down (remove workers)
    ↓ [Back to min workers]
Idle State
```

## Configuration

### Default Thresholds

| Parameter | Default | Description |
|-----------|---------|-------------|
| `min_workers` | 2 | Minimum workers (always active) |
| `max_workers` | CPU×2 | Maximum workers |
| `scale_up_threshold` | 100 | Queue depth to trigger scale up |
| `scale_down_threshold` | 10 | Queue depth to trigger scale down |
| `idle_timeout_ms` | 5000 | Scale down after 5s idle |
| `check_interval_ms` | 1000 | Check scaling every 1s |

### Custom Configuration

```python
# Set custom thresholds
gsyncio.set_auto_scaling(True)

# In scheduler.c, modify:
# WORKER_MANAGER_SCALE_UP_THRESHOLD
# WORKER_MANAGER_SCALE_DOWN_THRESHOLD
# WORKER_MANAGER_IDLE_TIMEOUT_MS
```

## Energy-Efficient Mode

When enabled, the worker manager:
- Reduces minimum workers to 1
- Scales down faster (2.5s idle timeout)
- More aggressive about removing idle workers

```python
# Enable for battery-powered devices or shared servers
gsyncio.set_energy_efficient_mode(True)
```

## Performance Impact

### Memory Savings

| Mode | Idle Workers | Memory Usage |
|------|--------------|--------------|
| **Default** | 2 workers | ~200KB |
| **Energy-efficient** | 1 worker | ~100KB |
| **Fixed (12 workers)** | 12 workers | ~1.2MB |

### CPU Usage

| Mode | Idle CPU | Under Load |
|------|----------|------------|
| **Default** | <1% | 100% |
| **Energy-efficient** | <0.5% | 100% |
| **Fixed** | 1-2% | 100% |

## Use Cases

### Dedicated Server

```python
# Maximize throughput
gsyncio.init_scheduler(num_workers=0)  # Auto-detect CPU
gsyncio.set_auto_scaling(True)
gsyncio.set_energy_efficient_mode(False)
```

### Shared Server

```python
# Leave room for other processes
gsyncio.init_scheduler(num_workers=4)  # Fixed, predictable
gsyncio.set_auto_scaling(False)
```

### Development Machine

```python
# Balance performance and resource usage
gsyncio.init_scheduler()
gsyncio.set_energy_efficient_mode(True)
```

### I/O-Bound Workload

```python
# Most time waiting - fewer workers needed
gsyncio.init_scheduler(num_workers=2)
gsyncio.set_auto_scaling(True)
gsyncio.set_energy_efficient_mode(True)
```

### CPU-Bound Workload

```python
# Maximize parallelism
gsyncio.init_scheduler(num_workers=0)  # All CPU cores
gsyncio.set_auto_scaling(True)
gsyncio.set_energy_efficient_mode(False)
```

## Monitoring Dashboard

```python
import gsyncio
import time

print("Worker Manager Dashboard")
print("=" * 50)

while True:
    util = gsyncio.get_worker_utilization()
    recommended = gsyncio.get_recommended_workers()
    current = gsyncio.num_workers()
    
    print(f"Utilization: {util:5.1f}% | "
          f"Current: {current} | "
          f"Recommended: {recommended}")
    
    time.sleep(1)
```

## Implementation Details

### Files

- `csrc/worker_manager.h` - Header file
- `csrc/worker_manager.c` - Implementation
- `csrc/scheduler.c` - Integration with scheduler
- `gsyncio/_gsyncio_core.pyx` - Python bindings
- `gsyncio/core.py` - Python wrapper

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│                  Python Application                      │
├─────────────────────────────────────────────────────────┤
│  Worker Manager API                                      │
│  - set_auto_scaling()                                   │
│  - set_energy_efficient_mode()                          │
│  - get_recommended_workers()                            │
│  - get_worker_utilization()                             │
├─────────────────────────────────────────────────────────┤
│  C Worker Manager                                        │
│  ┌─────────────────────────────────────────────────┐   │
│  │  Scaling Decision Engine                         │   │
│  │  - Monitor queue depth                          │   │
│  │  - Track idle time                              │   │
│  │  - Calculate utilization                        │   │
│  └─────────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────────┐   │
│  │  Worker Pool                                     │   │
│  │  - Add workers when busy                        │   │
│  │  - Remove workers when idle                     │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

## Troubleshooting

### Issue: Too many workers

```python
# Symptom: High memory usage, context switching
gsyncio.set_energy_efficient_mode(True)  # More aggressive scaling
gsyncio.init_scheduler(num_workers=4)    # Cap at 4
```

### Issue: Not scaling up

```python
# Symptom: High queue depth, slow processing
gsyncio.set_auto_scaling(True)           # Ensure enabled
# Check scale_up_threshold in worker_manager.h
```

### Issue: Frequent scaling

```python
# Symptom: Workers constantly added/removed
# Increase check interval or thresholds
# Modify WORKER_MANAGER_CHECK_INTERVAL_MS
```

## API Reference

### `set_auto_scaling(enabled: bool)`

Enable or disable automatic worker scaling.

```python
gsyncio.set_auto_scaling(True)   # Enable
gsyncio.set_auto_scaling(False)  # Disable
```

### `set_energy_efficient_mode(enabled: bool)`

Enable energy-efficient mode for reduced resource usage.

```python
gsyncio.set_energy_efficient_mode(True)   # Save resources
gsyncio.set_energy_efficient_mode(False)  # Max performance
```

### `get_worker_utilization() -> float`

Get current worker utilization percentage (0-100).

```python
util = gsyncio.get_worker_utilization()
print(f"Workers are {util:.1f}% utilized")
```

### `get_recommended_workers() -> int`

Get recommended number of workers based on current workload.

```python
recommended = gsyncio.get_recommended_workers()
print(f"System recommends {recommended} workers")
```

### `check_worker_scaling()`

Manually trigger a worker scaling check.

```python
gsyncio.check_worker_scaling()
```

## Summary

The Intelligent Worker Manager provides:

- ✅ **Automatic scaling** - No manual tuning needed
- ✅ **Resource efficiency** - Scale down when idle
- ✅ **Performance** - Scale up under load
- ✅ **Monitoring** - Real-time utilization metrics
- ✅ **Flexibility** - Configurable thresholds

**Default behavior is optimal for most cases.** Only adjust if you have specific requirements (shared server, I/O-bound, energy-constrained).
