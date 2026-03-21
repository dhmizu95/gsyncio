# gsyncio

High-performance fiber-based concurrency for Python.

## Overview

gsyncio is a Python package that provides Viper-like fiber-based concurrency primitives, designed to deliver significantly better performance than Python's built-in asyncio for high-concurrency workloads. It provides both fire-and-forget parallel work (task/sync model) and I/O-bound asynchronous operations (async/await model) with minimal overhead.

## Features

- **M:N Fiber Scheduler**: Maps millions of fibers onto a pool of worker threads with work-stealing for optimal load balancing
- **Task/Sync Model**: Fire-and-forget parallelism for CPU-bound and mixed workloads
- **Async/Await Model**: High-performance async I/O with full Python async syntax support
- **Channels**: Typed message-passing between fibers (like Go channels)
- **WaitGroups**: Synchronization primitive for waiting on multiple operations
- **Select Statements**: Multiplexing on multiple channel operations
- **Asyncio Compatibility**: Can monkey-patch asyncio for transparent performance improvements

## Installation

```bash
# From source
cd gsyncio
pip install -e .

# With development dependencies
pip install -e ".[dev]"
```

## Quick Start

### Task/Sync Model

```python
import gsyncio as gs

def worker(n):
    result = sum(range(n))
    print(f"Worker {n}: sum = {result}")

def main():
    # Spawn multiple tasks
    for i in range(100):
        gs.task(worker, 1000 * (i + 1))
    
    # Wait for all tasks to complete
    gs.sync()
    print("All workers completed!")

gs.run(main)
```

### Async/Await Model

```python
import gsyncio as gs

async def fetch_url(url):
    await gs.sleep(100)  # Simulate I/O
    return f"Data from {url}"

async def main():
    # Create all tasks concurrently
    urls = [f"http://example.com/{i}" for i in range(100)]
    tasks = [gs.create_task(fetch_url(url)) for url in urls]
    
    # Wait for all results
    results = await gs.gather(*tasks)
    print(f"Fetched {len(results)} URLs")

gs.run(main())
```

### Channels (Go-equivalent Producer/Consumer)

> **Note**: Channel `send`/`recv` use fiber parking under the hood, so both
> producer and consumer must be plain functions spawned with `gs.task()` — not
> `async def` coroutines. This mirrors Go's goroutine model.

```python
import gsyncio as gs

def producer(ch):
    for i in range(1, 6):
        ch.send_nowait(i)
        print(f"Sent: {i}")
    gs.close(ch)

def consumer(ch):
    # Mirrors Go's "for num := range ch" — drains until closed and empty
    while True:
        val = ch.recv_nowait()
        if val is not None:
            print(f"Received: {val}")
        elif ch.size == 0 and ch.closed:
            break
    print("Channel closed.")

def main():
    ch = gs.chan(5)  # buffered channel, like Go's make(chan int, 5)
    gs.task(producer, ch)
    gs.task(consumer, ch)
    gs.sync()

gs.run(main)
```

Output (order may interleave depending on scheduling):
```
Sent: 1
Sent: 2
...
Received: 1
Received: 2
...
Channel closed.
```

### WaitGroups

```python
import gsyncio as gs

async def worker(wg, worker_id):
    try:
        await gs.sleep(100)
        print(f"Worker {worker_id} completed")
    finally:
        gs.done(wg)

async def main():
    wg = gs.create_wg()
    gs.add(wg, 5)
    
    for i in range(5):
        gs.task(worker, wg, i)
    
    await gs.wait(wg)
    print("All workers completed")

gs.run(main())
```

### Select Statement

```python
import gsyncio as gs

async def server():
    auth_chan = gs.chan(10)
    data_chan = gs.chan(10)
    
    while True:
        result = await gs.select(
            gs.select_recv(auth_chan),
            gs.select_recv(data_chan),
            gs.default(lambda: None)
        )
        
        if result.channel == auth_chan:
            print(f"Auth: {result.value}")
        elif result.channel == data_chan:
            print(f"Data: {result.value}")

gs.run(server())
```

### Asyncio Compatibility

```python
import gsyncio
gsyncio.install()  # Replace asyncio event loop

import asyncio

async def main():
    await asyncio.sleep(1)
    return "done"

result = asyncio.run(main())  # Now uses gsyncio's scheduler
```

## API Reference

### Core Functions

| Function | Description |
|----------|-------------|
| `gs.task(func, *args)` | Spawn a fire-and-forget task |
| `gs.sync()` | Wait for all tasks to complete |
| `gs.sync_timeout(timeout)` | Wait for tasks with timeout |
| `gs.run(func)` | Run function in gsyncio runtime |
| `gs.create_task(coro)` | Create task from coroutine |
| `gs.sleep(ms)` | Async sleep for milliseconds |
| `gs.gather(*futures)` | Wait for multiple futures |
| `gs.chan(capacity)` | Create a channel |
| `gs.send(chan, value)` | Send to channel |
| `gs.recv(chan)` | Receive from channel |
| `gs.create_wg()` | Create WaitGroup |
| `gs.add(wg, n)` | Add to WaitGroup |
| `gs.done(wg)` | Signal completion to WaitGroup |
| `gs.wait(wg)` | Wait for WaitGroup |
| `gs.select(*cases)` | Multiplex on channels |

### Classes

| Class | Description |
|-------|-------------|
| `Future` | Represents a future value |
| `Channel` | Typed message-passing channel |
| `WaitGroup` | Synchronization primitive |
| `SelectResult` | Result of select operation |
| `AsyncRange` | Async iterator like range() |

## Performance

gsyncio is designed for high-concurrency workloads with the following goals:

| Metric | Target | Typical asyncio |
|--------|--------|-----------------|
| Context Switch Time | <1 µs | 50-100 µs |
| Memory per Task | <200 bytes | ~50 KB |
| Max Concurrent Tasks | 1M+ | ~100K |
| I/O Throughput | 2-10x improvement | baseline |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Python Application                        │
├─────────────────────────────────────────────────────────────┤
│  gsyncio Python Layer                                        │
│  ├── task.py      (Task/Sync model)                         │
│  ├── async_.py    (Async/Await model)                       │
│  ├── channel.py   (Channel operations)                      │
│  ├── waitgroup.py (WaitGroup operations)                    │
│  ├── select.py    (Select operations)                       │
│  └── future.py    (Future implementation)                   │
├─────────────────────────────────────────────────────────────┤
│  Cython Wrapper (_gsyncio_core.pyx)                         │
├─────────────────────────────────────────────────────────────┤
│  gsyncio C Core                                              │
│  ├── fiber.c      (Context switching)                       │
│  ├── scheduler.c  (M:N work-stealing scheduler)             │
│  ├── fiber_pool.c (Fiber object pool)                       │
│  ├── future.c     (Future implementation)                   │
│  ├── channel.c    (Channel implementation)                  │
│  ├── waitgroup.c  (WaitGroup implementation)                │
│  └── select.c     (Select implementation)                   │
└─────────────────────────────────────────────────────────────┘
```

## Examples

See the `examples/` directory for complete examples:

- `task_example.py` - Basic task/sync usage
- `async_example.py` - Async/await with gather
- `channel_example.py` - Channel-based communication
- `go_producer_consumer.py` - Go-equivalent producer/consumer (recommended channel pattern)
- `waitgroup_example.py` - WaitGroup synchronization
- `select_example.py` - Select statement multiplexing


See the `benchmarks/` directory for performance benchmarks and comparisons with Go coroutines.

## Running Tests

```bash
# Run all tests
pytest tests/ -v

# Run with coverage
pytest tests/ -v --cov=gsyncio

# Run specific test file
pytest tests/test_gsyncio.py -v
```

## Building from Source

```bash
# Install build dependencies
pip install cython setuptools

# Build the C extension
python setup.py build_ext --inplace

# Install in development mode
pip install -e .
```

## Requirements

- Python 3.8+
- C compiler (GCC, Clang, or MSVC)
- Cython 0.29+ (for C extension)

## License

MIT License - see LICENSE file for details.

## Contributing

Contributions are welcome! Please read our contributing guidelines before submitting PRs.

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests: `pytest tests/ -v`
5. Submit a pull request

## Acknowledgments

gsyncio draws inspiration from:

- Go's goroutines and channels
- Python's asyncio
- Viper language fiber runtime
- libuv event loop

## Future Work

- [ ] Full C-based event loop for async I/O
- [ ] TCP/UDP socket support
- [ ] File I/O operations
- [ ] DNS resolution
- [ ] SSL/TLS support
- [ ] Distributed computing capabilities
- [ ] Context cancellation
- [ ] Structured concurrency (task groups)
