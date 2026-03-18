# gsyncio vs asyncio vs Go Goroutines: Comprehensive Comparison

## Executive Summary

This document provides a detailed technical comparison between three concurrent programming approaches:

1. **gsyncio** - Python fiber-based concurrency library (M:N scheduler with work-stealing)
2. **asyncio** - Python's built-in async/await concurrency framework
3. **Go Goroutines** - Go's native lightweight concurrency primitive

---

## 1. Conceptual Architecture

### 1.1 gsyncio: Fiber-Based Hybrid Model

gsyncio implements a **hybrid M:N fiber scheduler** that combines concepts from both asyncio and Go:

```
┌─────────────────────────────────────────────────────────────────┐
│                      gsyncio Architecture                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐       │
│   │   Fiber 1   │    │   Fiber 2   │    │   Fiber N   │       │
│   │  (Python    │    │  (Python    │    │  (Python    │       │
│   │   code)     │    │   code)     │    │   code)     │       │
│   └──────┬──────┘    └──────┬──────┘    └──────┬──────┘       │
│          │                   │                   │              │
│          └───────────────────┼───────────────────┘              │
│                              ▼                                   │
│                    ┌─────────────────┐                           │
│                    │  M:N Scheduler  │                           │
│                    │ (Work-Stealing) │                           │
│                    └────────┬────────┘                           │
│                             │                                    │
│         ┌───────────────────┼───────────────────┐               │
│         ▼                   ▼                   ▼                │
│   ┌──────────┐        ┌──────────┐        ┌──────────┐         │
│   │ Worker   │        │ Worker   │        │ Worker   │         │
│   │ Thread 1 │        │ Thread 2 │        │ Thread M │         │
│   └──────────┘        └──────────┘        └──────────┘         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**Key Characteristics:**
- **M fibers to N threads** mapping (typically M >> N)
- **Work-stealing scheduler** for load balancing
- **Stackful coroutines** - each fiber has its own call stack
- **Dual API**: Fire-and-forget `task()` + async/await `create_task()`
- **Go-inspired channels**: `chan()`, `send()`, `recv()`
- **Select statements**: Multiplex on multiple channel operations

### 1.2 asyncio: Single-Threaded Event Loop

asyncio uses a **single-threaded cooperative multitasking** model:

```
┌─────────────────────────────────────────────────────────────────┐
│                      asyncio Architecture                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │                    Event Loop (Single Thread)            │   │
│   │  ┌─────────┐  ┌─────────┐  ┌─────────┐               │   │
│   │  │ Task 1  │  │ Task 2  │  │ Task N  │  ...            │   │
│   │  │(coro)   │  │(coro)   │  │(coro)   │               │   │
│   │  └────┬────┘  └────┬────┘  └────┬────┘               │   │
│   │       │            │            │                      │   │
│   │       └────────────┼────────────┘                      │   │
│   │                    ▼                                   │   │
│   │          ┌─────────────────┐                          │   │
│   │          │   Task Queue    │                          │   │
│   │          └────────┬────────┘                          │   │
│   │                   │                                    │   │
│   │    ┌──────────────┼──────────────┐                     │   │
│   │    ▼              ▼              ▼                      │   │
│   │ ┌──────┐   ┌──────────┐   ┌──────────┐               │   │
│   │ │ epoll│   │  Timer   │   │ callbacks│               │   │
│   │ │ /kqueue│   │  Wheel   │   │          │               │   │
│   │ └──────┘   └──────────┘   └──────────┘               │   │
│   └─────────────────────────────────────────────────────────┘   │
│                                                                  │
│   Operating System (kernel-level I/O multiplexing)              │
└─────────────────────────────────────────────────────────────────┘
```

**Key Characteristics:**
- **Single-threaded** event loop
- **Stackless coroutines** - `async`/`await` functions have no persistent stack
- **Event-driven I/O** using epoll (Linux), kqueue (macOS/BSD), or select
- **Cooperative multitasking** - tasks must explicitly yield with `await`
- **Future/Task-based** API
- **Proactor pattern** for I/O operations

### 1.3 Go Goroutines: Native Lightweight Threads

Go implements **M:N thread scheduler** at the language runtime level:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Go Goroutine Architecture                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐        │
│   │ Goroutine 1│    │ Goroutine 2 │    │ Goroutine N │        │
│   │   (Go      │    │   (Go       │    │   (Go       │        │
│   │   code)    │    │   code)     │    │   code)     │        │
│   └──────┬──────┘    └──────┬──────┘    └──────┬──────┘       │
│          │                   │                   │              │
│          └───────────────────┼───────────────────┘              │
│                              ▼                                   │
│                 ┌─────────────────────────┐                     │
│                 │    Go Scheduler (G)     │                     │
│                 │  (Part of Go Runtime)    │                     │
│                 └────────────┬────────────┘                     │
│                              │                                    │
│         ┌───────────────────┼───────────────────┐               │
│         ▼                   ▼                   ▼                │
│   ┌──────────┐        ┌──────────┐        ┌──────────┐          │
│   │   P1    │        │   P2    │        │   Pm    │          │
│   │ (Proc)  │        │ (Proc)  │        │ (Proc)  │          │
│   └────┬─────┘        └────┬─────┘        └────┬─────┘        │
│        │                   │                   │               │
│        └───────────────────┼───────────────────┘               │
│                            ▼                                    │
│   ┌──────────┐        ┌──────────┐        ┌──────────┐        │
│   │   M1    │        │   M2    │        │   Mm    │        │
│   │ (Thread)│        │ (Thread)│        │ (Thread)│        │
│   └──────────┘        └──────────┘        └──────────┘        │
│                                                                  │
│   OS Kernel                                                     │
└─────────────────────────────────────────────────────────────────┘
```

**Key Characteristics:**
- **M goroutines to M threads** (via P processors)
- **Preemptive scheduling** via function prologue instrumentation
- **Stackful** - each goroutine has growing stack (starts at 2KB)
- **Integrated runtime** with garbage collection
- **Channel-based** communication (built into language)
- **Select statement** for channel multiplexing

---

## 2. Technical Implementation Details

### 2.1 Context Switching

| Aspect | gsyncio | asyncio | Go Goroutines |
|--------|---------|---------|---------------|
| **Mechanism** | Assembly-based `sigjmp_buf` | Python generator-based | Go runtime assembly |
| **Stack Type** | Stackful (4KB-32KB) | Stackless (no persistent stack) | Stackful (2KB-GB) |
| **Switch Time** | <1 µs (target) | 50-100 µs | ~0.5 µs |
| **Per-Task Memory** | <200 bytes (target) | ~50 KB | ~2-8 KB |
| **Max Concurrent** | 1M+ (target) | ~100K (practical) | 100K-1M+ |

### 2.2 Scheduler Design

**gsyncio:**
- M:N work-stealing scheduler
- Per-thread double-ended queue (deque)
- Fibers stolen from busy threads when idle
- Supports both fire-and-forget and awaitable tasks

**asyncio:**
- Single-threaded event loop
- Single queue for ready tasks
- Event-driven I/O multiplexing
- No work-stealing (single thread)

**Go:**
- M:P:N scheduler (M goroutines, P processors, N threads)
- Per-P local run queue + global queue
- Work stealing between P's
- Integrated with GC

### 2.3 I/O Model

| Aspect | gsyncio | asyncio | Go |
|--------|---------|---------|-----|
| **I/O Multiplexing** | epoll/io_uring (Linux) | epoll/kqueue/select | Netpoller (epoll/kqueue) |
| **Blocking I/O** | Yields fiber, not thread | Blocks event loop (should avoid) | Blocks goroutine, not thread |
| **Async File I/O** | Planned | aiocore/aiosocket | `os.File` methods are blocking* |
| **DNS Resolution** | Planned | `getaddrinfo` async | Blocking by default |

*Go 1.11+ has `net/http` with async capabilities

---

## 3. API Comparison

### 3.1 Task/Coroutine Creation

```python
# gsyncio - Fire-and-forget task
import gsyncio as gs

def worker(n):
    result = sum(range(n))
    print(f"Result: {result}")

gs.task(worker, 10000)  # Spawn immediately
gs.sync()  # Wait for completion
```

```python
# gsyncio - Async/await task
import gsyncio as gs

async def fetch(url):
    await gs.sleep(100)  # Simulate I/O
    return f"Data from {url}"

async def main():
    task = gs.create_task(fetch("http://example.com"))
    result = await task

gs.run(main())
```

```python
# asyncio - Standard approach
import asyncio

async def fetch(url):
    await asyncio.sleep(0.1)  # Simulate I/O
    return f"Data from {url}"

async def main():
    task = asyncio.create_task(fetch("http://example.com"))
    result = await task

asyncio.run(main())
```

```go
// Go - Goroutine
package main

import (
    "fmt"
    "time"
)

func worker(n int) int {
    result := 0
    for i := 0; i < n; i++ {
        result += i
    }
    return result
}

func main() {
    done := make(chan int, 1)
    go func() {
        done <- worker(10000)
    }()
    result := <-done
    fmt.Printf("Result: %d\n", result)
}
```

### 3.2 Concurrent Task Execution

```python
# gsyncio - Fire-and-forget parallelism
import gsyncio as gs

def cpu_task(n):
    return sum(range(n))

def main():
    for i in range(1000):
        gs.task(cpu_task, 1000)
    gs.sync()  # Wait for all

gs.run(main)
```

```python
# asyncio - Async gather
import asyncio

async def async_task(n):
    await asyncio.sleep(0)  # Simulate async work
    return sum(range(n))

async def main():
    tasks = [async_task(1000) for _ in range(1000)]
    results = await asyncio.gather(*tasks)

asyncio.run(main())
```

```go
// Go - WaitGroup
package main

import (
    "sync"
)

func worker(n int, wg *sync.WaitGroup) {
    defer wg.Done()
    sum := 0
    for i := 0; i < n; i++ {
        sum += i
    }
}

func main() {
    var wg sync.WaitGroup
    for i := 0; i < 1000; i++ {
        wg.Add(1)
        go worker(1000, &wg)
    }
    wg.Wait()
}
```

### 3.3 Channel Communication

```python
# gsyncio - Channels (Go-style)
import gsyncio as gs

async def producer(chan):
    for i in range(10):
        await gs.send(chan, i)

async def consumer(chan):
    for _ in range(10):
        value = await gs.recv(chan)
        print(f"Received: {value}")

async def main():
    ch = gs.chan(5)  # Buffered channel
    gs.task(producer, ch)
    gs.task(consumer, ch)

gs.run(main())
```

```go
// Go - Channels
package main

import "fmt"

func producer(ch chan int) {
    for i := 0; i < 10; i++ {
        ch <- i
    }
    close(ch)
}

func consumer(ch chan int) {
    for v := range ch {
        fmt.Printf("Received: %d\n", v)
    }
}

func main() {
    ch := make(chan int, 5)
    go producer(ch)
    go consumer(ch)
    // Wait using channel or time.Sleep
}
```

### 3.4 Select/Multiplexing

```python
# gsyncio - Select statement
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

```go
// Go - Select statement
package main

import "fmt"

func server(authChan, dataChan chan string) {
    for {
        select {
        case msg := <-authChan:
            fmt.Printf("Auth: %s\n", msg)
        case msg := <-dataChan:
            fmt.Printf("Data: %s\n", msg)
        default:
            // Do nothing
        }
    }
}
```

```python
# asyncio - Using asyncio.wait and asyncio.gather
# No native select, must use gather or TaskGroup (Python 3.11+)
import asyncio

async def worker(chan):
    async for value in chan:
        print(f"Received: {value}")

async def main():
    auth_chan = asyncio.Queue()
    data_chan = asyncio.Queue()
    
    # Can't easily "select" - use gather with shared cancel
    # This is a limitation of asyncio vs Go channels
    pass
```

---

## 4. Performance Characteristics

### 4.1 Target Performance Goals (from gsyncio README)

| Metric | gsyncio Target | Typical asyncio | Go Goroutines |
|--------|---------------|-----------------|---------------|
| Context Switch Time | <1 µs | 50-100 µs | ~0.5 µs |
| Memory per Task | <200 bytes | ~50 KB | ~2-8 KB |
| Max Concurrent Tasks | 1M+ | ~100K | 100K-1M+ |
| I/O Throughput | 2-10x asyncio | baseline | High |
| HTTP Server RPS | 100K+ (target) | 20K | 50K-100K |

### 4.2 When to Use Each

**Use gsyncio when:**
- You need Python compatibility with better-than-asyncio performance
- You have millions of concurrent I/O-bound tasks
- You want Go-style channels and select in Python
- You need both fire-and-forget (CPU) and async I/O in same codebase
- You want asyncio compatibility (monkey-patch option)

**Use asyncio when:**
- Building standard Python async applications
- Compatibility with async ecosystem (aiohttp, asyncpg, etc.) is critical
- Simplicity is preferred over maximum performance
- You need built-in SSL/TLS support

**Use Go when:**
- Building high-performance backend services
- You need true parallelism (multiple CPU cores)
- You want built-in channel primitives in the language
- GC is acceptable (or desired) for your use case
- You need excellent stdlib and tooling

---

## 5. Feature Comparison Matrix

| Feature | gsyncio | asyncio | Go |
|---------|---------|---------|-----|
| **Task Creation** | `gs.task()` | `asyncio.create_task()` | `go func()` |
| **Async/Await** | ✅ | ✅ | N/A (no async keyword) |
| **Fire-and-Forget** | ✅ | ❌ | ✅ |
| **Channels** | ✅ | ❌ | ✅ |
| **Select Statement** | ✅ | ❌ (limited) | ✅ |
| **WaitGroups** | ✅ | ❌ | ✅ (sync.WaitGroup) |
| **Futures** | ✅ | ✅ | ✅ (via channels) |
| **Mutex/RWMutex** | Planned | ❌ | ✅ |
| **Thread Pool** | Internal | External | Built-in |
| **Preemptive** | No* | No | Yes |
| **True Parallelism** | Limited** | No | Yes |
| **GC** | Python GC | Python GC | Go GC |

*gsyncio can be configured for cooperative only
**gsyncio can use multiple threads for CPU-bound work

---

## 6. Migration Paths

### 6.1 asyncio → gsyncio

```python
# Before: asyncio
import asyncio

async def main():
    await asyncio.sleep(1)
    return "done"

asyncio.run(main())

# After: gsyncio (minimal changes)
import gsyncio as gs

async def main():
    await gs.sleep(1000)  # Note: milliseconds, not seconds
    return "done"

gs.run(main())

# Or: Use gsyncio.install() to monkey-patch asyncio
import gsyncio
gsyncio.install()  # After this, all asyncio code uses gsyncio

import asyncio
async def main():
    await asyncio.sleep(1)
    return "done"

asyncio.run(main())  # Now uses gsyncio!
```

### 6.2 Go → gsyncio (Python developers)

```go
// Go code
package main

import (
    "fmt"
    "sync"
)

func worker(id int, wg *sync.WaitGroup) {
    defer wg.Done()
    fmt.Printf("Worker %d done\n", id)
}

func main() {
    var wg sync.WaitGroup
    for i := 0; i < 10; i++ {
        wg.Add(1)
        go worker(i, &wg)
    }
    wg.Wait()
}
```

```python
# Equivalent gsyncio code
import gsyncio as gs

def worker(id):
    print(f"Worker {id} done")

def main():
    for i in range(10):
        gs.task(worker, i)
    gs.sync()

gs.run(main)
```

---

## 7. Summary Comparison

| Dimension | gsyncio | asyncio | Go Goroutines |
|-----------|---------|---------|---------------|
| **Paradigm** | Hybrid: fibers + async | Async-first | Concurrent threads |
| **Language** | Python | Python | Go |
| **Performance** | High (target: 10x asyncio) | Moderate | Very High |
| **Ease of Use** | Good | Excellent | Good |
| **Ecosystem** | Growing | Excellent (mature) | Excellent (stdlib) |
| **Learning Curve** | Low (Python syntax) | Low (Python syntax) | Medium (Go syntax) |
| **Best For** | High-concurrency Python | Standard Python async | High-performance services |

---

## 8. Conclusion

Each concurrency model has its place:

- **gsyncio** bridges the gap between Python asyncio and Go-style concurrency, offering Go-like primitives (channels, select, goroutines) within Python while targeting performance significantly better than asyncio.

- **asyncio** remains the standard for Python async programming with the largest ecosystem and easiest integration, best for typical async applications where maximum performance isn't critical.

- **Go goroutines** are the gold standard for high-performance concurrent programming, with the best raw performance and most mature implementation of the M:N scheduling model.

For Python developers wanting Go-like concurrency without leaving Python, gsyncio provides an compelling option that maintains Python syntax while implementing the architectural patterns that make Go successful.
