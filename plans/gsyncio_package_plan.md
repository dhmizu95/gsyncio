# gsyncio: High-Performance Fiber-Based Concurrency Package for Python

## Overview
gsyncio is a Python package that provides Viper-like fiber-based concurrency primitives, designed to override and replace Python's asyncio with significantly better performance. The core is implemented in C for maximum speed, providing both fire-and-forget parallel work (`task`/`sync`) and I/O-bound asynchronous operations (`async`/`await`) with minimal overhead. It includes full support for channels, waitgroups, select statements, and complete asyncio-compatible syntax.

## Package Structure
```
gsyncio/
├── gsyncio/
│   ├── __init__.py
│   ├── _core.pyx          # Cython wrapper for C core (exposes C API to Python)
│   ├── core.py            # Pure Python fallback/core interface
│   ├── task.py            # task/sync model implementation
│   ├── async_.py          # async/await model implementation
│   ├── channel.py         # Channel implementation (send/recv)
│   ├── waitgroup.py       # WaitGroup implementation
│   ├── select.py          # Select statement implementation
│   ├── scheduler.py       # Fiber scheduler interface
│   ├── future.py          # Future implementation for async/await
│   └── utils.py           # Utility functions
├── csrc/                  # C source code for high-performance core
│   ├── fiber.h            # Fiber (stackful coroutine) interface
│   ├── fiber.c            # Fiber implementation
│   ├── scheduler.h        # M:N work-stealing scheduler interface
│   ├── scheduler.c        # Scheduler implementation
│   ├── fiber_pool.h       # Fiber pool interface
│   ├── fiber_pool.c       # Fiber pool implementation
│   ├── future.h           # Future interface
│   ├── future.c           # Future implementation
│   ├── event_loop.h       # Event loop interface
│   ├── channel.h          # Channel interface
│   ├── channel.c          # Channel implementation
│   ├── waitgroup.h        # WaitGroup interface
│   ├── waitgroup.c        # WaitGroup implementation
│   └── select.h           # Select interface
│       └── select.c       # Select implementation
├── tests/
│   ├── test_task.py
│   ├── test_async.py
│   ├── test_channel.py
│   ├── test_waitgroup.py
│   ├── test_select.py
│   ├── test_scheduler.py
│   └── test_integration.py
├── examples/
│   ├── task_example.py
│   ├── async_example.py
│   ├── pipeline_example.py
│   ├── channel_example.py
│   ├── waitgroup_example.py
│   ├── select_example.py
│   └── performance_comparison.py
├── setup.py
├── README.md
└── LICENSE
```

## Core Design Principles

### 1. C-Based High-Performance Core
- All performance-critical components implemented in C
- Stackful coroutines using platform-specific assembly for context switching
- M:N work-stealing scheduler with per-thread deques
- Lock-free data structures where possible
- Minimal memory allocations through object pooling

### 2. Asyncio Replacement Design
- Designed to be a drop-in replacement for asyncio
- Similar API but with significantly better performance
- Event loop compatible with asyncio interfaces
- Future and Task classes that mirror asyncio semantics
- Can override asyncio.get_event_loop() to use gsyncio's event loop

### 3. Dual Concurrency Models
- **Task/Sync Model**: Fire-and-forget parallel work (like Go goroutines)
- **Async/Await Model**: I/O-bound operations with results (like asyncio but faster)

### 4. Communication Primitives
- **Channels**: Typed message-passing between fibers (like Go channels)
- **WaitGroups**: Synchronization for waiting on multiple fiber completions
- **Select Statement**: Multiplexing on channel operations

## Syntax Comparison Table

| Feature | gsyncio Syntax | Asyncio Syntax | Description |
|---------|---------------|----------------|-------------|
| **Task Creation** | `gs.task(func, *args)` | `asyncio.create_task(coro())` | Fire-and-forget parallel work |
| **Wait for Tasks** | `gs.sync()` | `asyncio.gather(*tasks)` | Wait for all spawned tasks |
| **Async Function** | `async def func():` | `async def func():` | Define async function |
| **Await** | `await future` | `await future` | Suspend until future ready |
| **Async For** | `async for x in iter:` | `async for x in iter:` | Async iteration |
| **Async With** | `async with ctx:` | `async with ctx:` | Async context manager |
| **Sleep** | `await gs.sleep(ms)` | `await asyncio.sleep(ms)` | Asynchronous sleep |
| **Async Range** | `async for i in gs.async_range(n):` | `async for i in asyncio.sleep(0):`* | Async iterator |
| **Gather** | `await gs.gather(*futures)` | `await asyncio.gather(*futures)` | Wait for multiple futures |
| **Future Retain** | `gs.ensure_future(future)` | `asyncio.ensure_future(future)` | Retain future reference |
| **Create Task** | `gs.create_task(coro())` | `asyncio.create_task(coro())` | Wrap coroutine in task |
| **Run Entry Point** | `gs.run(main())` | `asyncio.run(main())` | Entry point for async programs |
| **Channel Create** | `gs.chan(size)` | N/A | Create typed channel |
| **Channel Send** | `await gs.send(chan, value)` | N/A | Send value to channel |
| **Channel Recv** | `value = await gs.recv(chan)` | N/A | Receive value from channel |
| **Channel Close** | `gs.close(chan)` | N/A | Close channel |
| **WaitGroup Create** | `gs.create_wg()` | N/A | Create wait group |
| **WG Add** | `gs.add(wg, n)` | N/A | Add to wait group counter |
| **WG Done** | `gs.done(wg)` | N/A | Decrement wait group counter |
| **WG Wait** | `gs.wait(wg)` | N/A | Wait for wait group counter to reach zero |
| **Select** | `await gs.select(case1, case2, ...)` | N/A | Multiplex on channel operations |
| **Select Case Recv** | `gs.recv(chan)` | N/A | Receive case in select |
| **Select Case Send** | `gs.send(chan, value)` | N/A | Send case in select |
| **Select Default** | `gs.default()` | N/A | Default case in select |
| **Monkey-patch** | `gs.install()` | N/A | Replace asyncio event loop |
| **Event Loop Policy** | `gs.event_loop.set_event_loop_policy(policy)` | `asyncio.set_event_loop_policy(policy)` | Set event loop policy |

*Note: asyncio doesn't have a direct equivalent to async_range; this shows how it might be approximated

## Implementation Approach

### Phase 1: C Core Implementation
1. Implement fiber context switching in C using platform-specific techniques:
   - Linux/x86_64: Assembly-based setjmp/longjmp or libco
   - macOS: Similar approach with platform-specific adjustments
   - Windows: Fibers API or custom context switching
2. Create fiber pool for efficient allocation/reuse
3. Implement M:N work-stealing scheduler:
   - N worker threads (configurable, default = CPU cores)
   - Per-thread double-ended queues (deques)
   - Work-stealing algorithm for load balancing
   - Local vs external push operations for cache locality
4. Implement Future class with state tracking and callback mechanism
5. Create basic event loop with timer functionality
6. Implement channel primitives (create, send, recv, close)
7. Implement waitgroup primitives (create, add, done, wait)
8. Implement select statement support

### Phase 2: Python Interface and Task/Sync Model
1. Create Cython wrapper (_core.pyx) to expose C API to Python
2. Implement task.py with `task()` function that spawns fibers via C API
3. Implement sync() function to wait for task completion
4. Add task grouping and synchronization primitives
5. Test with CPU-bound workloads to verify performance

### Phase 3: Async/Await Infrastructure
1. Implement async_.py with:
   - `async def` decorator/transformation
   - `await` as fiber suspension primitive
   - `async for` async iteration support
   - `async with` async context manager support
2. Implement built-in async functions:
   - `sleep(ms)` - suspend for milliseconds
   - `async_range(n)` - async iterator 0..n-1
   - `gather(*futures)` - wait for multiple futures
   - `future_retain(future)` - retain future reference
3. Future methods: `is_ready()`, `result()`, `add_callback()`
4. Integrate with C core for fiber suspension/resumption

### Phase 4: Communication Primitives
1. Implement channel.py with:
   - `Chan[T]` generic type
   - `chan(size)` constructor
   - `send(chan, value)` operation
   - `recv(chan)` operation
   - `close(chan)` operation
2. Implement waitgroup.py with:
   - `WaitGroup()` constructor
   - `add(wg, n)` operation
   - `done(wg)` operation
   - `wait(wg)` operation
3. Implement select.py with:
   - `select()` statement support
   - Case patterns: `recv(chan)`, `send(chan, value)`, `default`
4. Integrate all with C core for fiber suspension/resumption

### Phase 5: Event Loop and I/O Integration
1. Implement event loop in C with selector integration (epoll/kqueue/select)
2. Add comprehensive timer functionality (timeouts, intervals)
3. Create async I/O primitives in C:
   - TCP/UDP sockets
   - File operations
   - Signal handling
4. Integrate I/O operations with fiber scheduler:
   - Blocking I/O operations yield fiber until ready
   - Use callback-based I/O that resumes fibers on completion
5. Provide Python interface matching asyncio's event loop API

### Phase 6: Asyncio Compatibility and Override
1. Implement asyncio-compatible interfaces:
   - EventLoopPolicy and EventLoop implementations
   - Future and Task classes matching asyncio semantics
   - Handle, TimerHandle, and other asyncio constructs
2. Provide monkey-patching capability to replace asyncio:
   ```python
   import gsyncio
   gsyncio.install()  # Replaces asyncio event loop with gsyncio's
   ```
3. Ensure compatibility with popular asyncio-based libraries
4. Test with aiohttp, asyncpg, and other asyncio libraries

### Phase 7: Optimization and Advanced Features
1. Implement fiber stack growth/shrinking for memory efficiency
2. Add priority-based scheduling for QoS control
3. Implement context cancellation (like Python's asyncio)
4. Add structured concurrency (task groups, nurseries)
5. Performance profiling and bottleneck elimination
6. Advanced features:
   - Distributed computing capabilities
   - Integration with data processing frameworks (Apache Arrow, etc.)

## API Design

### Task/Sync Model (Fire-and-Forget)
```python
import gsyncio as gs

def worker(n):
    # CPU-bound work - returns nothing in task model
    result = 0
    for i in range(n):
        result += i
    # Return value is ignored (fire-and-forget)

def main():
    # Spawn multiple concurrent tasks
    for i in range(1000):
        gs.task(worker, 1000)  # Spawn 1000 tasks
    
    # Wait for all tasks to complete
    gs.sync()
    
    print("All 1000 tasks completed")

if __name__ == "__main__":
    main()
```

### Async/Await Model (Asyncio Replacement)
```python
import gsyncio as gs
import time

# This will override asyncio when gsyncio is installed
async def fetch_url(url):
    # Simulate I/O operation - yields fiber until ready
    await gs.sleep(100)  # 100ms delay
    return f"Data from {url}"

async def process_data(data):
    # Simulate processing
    await gs.sleep(50)
    return f"Processed: {data}"

async def main():
    # Create all futures first (maximizes concurrency)
    urls = [f"http://example.com/{i}" for i in range(100)]
    tasks = [gs.create_task(fetch_url(url)) for url in urls]
    
    # Wait for all results
    results = await gs.gather(*tasks)
    
    # Process results concurrently
    process_tasks = [gs.create_task(process_data(result)) for result in results]
    processed_results = await gs.gather(*process_tasks)
    
    print(f"Processed {len(processed_results)} items")

# Standard asyncio entry point - now uses gsyncio's event loop
if __name__ == "__main__":
    gs.run(main())  # Or: asyncio.run(main()) after gsyncio.install()
```

### Channel Operations
```python
import gsyncio as gs

async def worker(chan_id, send_chan, recv_chan):
    # Worker receives data, processes it, and sends result
    data = await gs.recv(recv_chan)
    processed = f"Worker {chan_id}: {data}"
    await gs.send(send_chan, processed)

async def main():
    # Create channels for communication
    recv_chan = gs.chan(10)  # Buffered channel with capacity 10
    send_chan = gs.chan(10)
    
    # Spawn worker tasks
    for i in range(5):
        gs.task(worker, i, send_chan, recv_chan)
    
    # Send data to workers
    for i in range(5):
        await gs.send(recv_chan, f"Task {i}")
    
    # Receive results from workers
    for i in range(5):
        result = await gs.recv(send_chan)
        print(result)
    
    # Clean up channels
    gs.close(recv_chan)
    gs.close(send_chan)

if __name__ == "__main__":
    gs.run(main())
```

### WaitGroup Synchronization
```python
import gsyncio as gs

def worker(wg, worker_id):
    try:
        # Simulate work
        await gs.sleep(100)
        print(f"Worker {worker_id} completed")
    finally:
        # Signal completion
        gs.wg_done(wg)

async def main():
    # Create wait group
    wg = gs.WaitGroup()
    
    # Add worker count
    gs.wg_add(wg, 5)
    
    # Spawn workers
    for i in range(5):
        gs.task(worker, wg, i)
    
    # Wait for all workers to complete
    gs.wg_wait(wg)
    print("All workers completed")

if __name__ == "__main__":
    gs.run(main())
```

### Select Statement
```python
import gsyncio as gs

async def server():
    # Create channels for different types of requests
    auth_chan = gs.chan(10)
    data_chan = gs.chan(10)
    control_chan = gs.chan(10)
    
    async def auth_handler():
        while True:
            creds = await gs.recv(auth_chan)
            # Process authentication...
            await gs.send(auth_chan, f"Auth result for {creds}")
    
    async def data_handler():
        while True:
            request = await gs.recv(data_chan)
            # Process data request...
            await gs.send(data_chan, f"Data for {request}")
    
    async def control_handler():
        while True:
            cmd = await gs.recv(control_chan)
            # Process control command...
            await gs.send(control_chan, f"Control result for {cmd}")
    
    # Spawn handlers
    gs.task(auth_handler)
    gs.task(data_handler)
    gs.task(control_handler)
    
    # Main server loop with select
    while True:
        await gs.select(
            gs.recv(auth_chan),      # Case 1: receive on auth channel
            gs.recv(data_chan),      # Case 2: receive on data channel
            gs.recv(control_chan),   # Case 3: receive on control channel
            default=lambda: None     # Default case
        )

async def main():
    gs.task(server)
    # Run for a while then exit
    await gs.sleep(5000)

if __name__ == "__main__":
    gs.run(main())
```

### Asyncio Override/Compatibility
```python
# Option 1: Direct usage
import gsyncio as gs

async def my_coroutine():
    await gs.sleep(1)
    return "done"

result = gs.run(my_coroutine())

# Option 2: Monkey-patch to replace asyncio
import gsyncio
gsyncio.install()  # Replaces asyncio's event loop

import asyncio  # Now uses gsyncio's implementation under the hood

async def my_coroutine():
    await asyncio.sleep(1)
    return "done"

result = asyncio.run(my_coroutine())

# Option 3: Selective replacement
import gsyncio.event_loop
gsyncio.event_loop.set_event_loop_policy(gsyncio.event_loop.gsyncioEventLoopPolicy())
```

## Performance Characteristics

### Expected Improvements Over Asyncio:
- **Context Switching**: <1 microsecond vs ~50-100 microseconds in asyncio
- **Memory Overhead**: <200 bytes per task vs ~50KB per task in asyncio
- **Scalability**: 1M+ concurrent tasks vs ~100K practical limit in asyncio
- **I/O Throughput**: 2-10x improvement for high-concurrency I/O workloads
- **CPU Utilization**: Better cache locality and reduced syscall overhead

### Benchmark Goals:
| Metric | Target gsyncio | Typical Asyncio | Improvement |
|--------|---------------|-----------------|-------------|
| Context Switch Time | <1 µs | 50-100 µs | 50-100x |
| Memory per Task | <200 bytes | ~50 KB | 250x |
| Max Concurrent Tasks | 1M+ | ~100K | 10x |
| HTTP Server RPS | 100K+ | 20K | 5x |
| Pipeline Latency | <10 µs | ~100 µs | 10x |
| Channel Send/Recv | <0.5 µs | ~50 µs | 100x |
| WaitGroup Operations | <0.5 µs | ~20 µs | 40x |

## Platform Support
- **Primary**: Linux x86_64 (optimized assembly context switching)
- **Secondary**: macOS x86_64/ARM64, Windows x86_64
- **Fallback**: Portable C setjmp/longjmp for other platforms
- **Python**: 3.8+ (targeting 3.9+ for asyncio compatibility features)

## Dependencies
- **Build**: C compiler (GCC/clang/MSVC), Cython, setuptools
- **Runtime**: Python standard library only
- **Optional**: libco for portable context switching on some platforms
- **Testing**: pytest, benchmarking tools

## Installation
```bash
pip install gsyncio
# Or for development:
pip install -e .[dev]
```

## Usage Examples
See `examples/` directory for:
- Basic task/sync parallelism
- Async/await I/O operations
- Pipeline patterns
- Channel communication patterns
- WaitGroup synchronization
- Select statement multiplexing
- Performance comparisons vs asyncio/threading/multiprocessing
- Migration guides from asyncio

## Future Extensions
- Distributed fiber clusters
- GPU-accelerated fiber scheduling
- Real-time scheduling guarantees
- Hardware-assisted context switching
- Integration with data processing frameworks (Apache Arrow, etc.)

## Phase 8: Go-Style Coroutine Features (Future Implementation)

### Additional Synchronization Primitives
1. **Mutex** (`gsync.Mutex`)
   - Basic mutual exclusion lock
   - `lock()`, `unlock()`, `try_lock()`

2. **RWMutex** (`gsync.RWMutex`)
   - Reader-writer lock for read-heavy workloads
   - `lock()`, `unlock()`, `rlock()`, `runlock()`, `try_rlock()`

3. **Once** (`gsync.Once`)
   - Execute exactly once
   - `do(func)` - Call function exactly once

4. **Cond** (`gsync.Cond`)
   - Condition variable for signaling
   - `wait()`, `signal()`, `broadcast()`

5. **Pool** (`gsync.Pool`)
   - Object pool for reducing allocations
   - `get()`, `put()`

6. **Map** (`gsync.Map`)
   - Concurrent map operations
   - `load()`, `store()`, `delete()`, `range()`

7. **Atomic** (`gsync.atomic`)
   - Atomic operations
   - `add()`, `load()`, `store()`, `swap()`, `compare_exchange()`

### Advanced Task Management

8. **ErrGroup** (`gsync.ErrGroup`)
   - Parallel tasks with error handling
   - `go(func)`, `wait()` - Returns first error or nil

9. **Semaphore** (`gsync.Semaphore`)
   - Resource limiting
   - `acquire()`, `release()` - Limit concurrent operations

10. **Context** (`gsync.Context`)
    - Cancellation and deadlines
    - `with_cancel()`, `with_timeout()`, `with_deadline()`, `with_value()`

## Phase 9: Synchronous I/O (syncio Features) (Future Implementation)

### File I/O Operations
1. **Async File Operations**
   - `gsyncio.open()` - Async file open
   - `gsyncio.read()` - Async file read
   - `gsyncio.write()` - Async file write
   - `gsyncio.seek()` - Async file seek
   - `gsyncio.close()` - Async file close

### Network I/O Operations
2. **Async Socket Operations**
   - `gsyncio.connect()` - Async TCP connect
   - `gsyncio.accept()` - Async TCP accept
   - `gsyncio.send()` - Async socket send
   - `gsyncio.recv()` - Async socket receive
   - `gsyncio.udp_socket()` - Async UDP socket

3. **Async DNS Lookup**
   - `gsyncio.gethostbyname()` - Async DNS resolution
   - `gsyncio.getaddrinfo()` - Async address lookup

### Time Operations
4. **Async Time Functions**
   - `gsyncio.sleep()` - Async sleep (already implemented)
   - `gsyncio.timeout()` - Run with timeout
   - `gsyncio.wait_for()` - Wait for future with timeout

### Signal Handling
5. **Async Signal Handling**
   - `gsyncio.signal()` - Async signal handler
   - `gsyncio.ignore_signal()` - Ignore signals

### Additional I/O Patterns
6. **Pipe Operations**
   - `gsyncio.pipe()` - Create async pipe

7. **Subprocess Management**
   - `gsyncio.create_subprocess()` - Async subprocess creation
   - `gsyncio.wait_process()` - Async process waiting

8. **Memory-mapped Files**
   - `gsyncio.mmap()` - Async memory-mapped file operations