# gsyncio Improvement Suggestions

## Overview
This document outlines areas for improvement in the gsyncio library based on code analysis and test results. gsyncio aims to provide high-performance fiber-based concurrency for Python, but several issues prevent it from reaching its full potential.

## Critical Issues (Requires Immediate Fix)

### 1. Sleep Function Timing Issue
- **Location**: `gsyncio/async_.py:83`
- **Problem**: The `sleep()` function calls `sleep_ns(ms * 1000000)` but tests show it's sleeping for microseconds instead of milliseconds
- **Impact**: Breaks timing-dependent functionality and makes performance claims unreliable
- **Evidence**: Test failure `test_sleep` showed actual sleep time of ~5.5µs instead of expected 100ms

### 2. Future.__await__ Implementation Error
- **Location**: `gsyncio/future.py:152`
- **Problem**: `TypeError: cannot 'yield from' a coroutine object in a non-coroutine generator`
- **Root Cause**: Using `yield from event.wait()` where `event.wait()` returns a coroutine object
- **Impact**: Breaks core async/await functionality for Futures
- **Evidence**: Test failure `test_future_await`

## High Priority Improvements

### 3. Incomplete Monkey-Patching Implementation
- **Location**: `gsyncio/__init__.py:154-157`
- **Problem**: The `install()` function doesn't actually replace asyncio's event loop with gsyncio's fiber-based implementation
- **Current State**: Returns a standard asyncio event loop, providing no performance benefits
- **Impact**: Users cannot get automatic performance improvements for existing asyncio code
- **Requirement**: Needs full implementation that returns gsyncio's fiber-based event loop

### 4. Pure Python Fallback Performance Issues
- **Location**: `gsyncio/core.py` (Channel and WaitGroup implementations)
- **Problems**:
  - Channel implementation uses asyncio events internally, defeating the purpose of fiber-based concurrency
  - WaitGroup uses threading.Condition instead of fiber-based synchronization
- **Impact**: Pure Python fallback performs poorly and doesn't demonstrate the intended architecture
- **Evidence**: Channel's `_wait_for_space()` and `_wait_for_data()` methods create asyncio events

## Medium Priority Improvements

### 5. Scheduler Integration in Async/Await Model
- **Location**: `gsyncio/async_.py`
- **Problems**:
  - `_run_coroutine()` function still relies on asyncio's event loop
  - `create_task()` uses asyncio.ensure_future() instead of direct fiber scheduling
- **Impact**: Prevents achieving the full performance benefits of gsyncio's M:N fiber scheduler
- **Opportunity**: Deeper integration with gsyncio's scheduler for true fiber-based coroutines

### 6. Missing Features from Roadmap
Based on the README's "Future Work" section, these are not yet implemented:
- Full C-based event loop for async I/O
- TCP/UDP socket support
- File I/O operations
- DNS resolution
- SSL/TLS support
- Distributed computing capabilities
- Context cancellation
- Structured concurrency (task groups)

## Specific Fixes Needed

### Fix for Sleep Function
Verify that the C extension's `sleep_ns` function works correctly and that the time conversion is proper. The issue might be in the C implementation or in how the time is being measured in tests.

### Fix for Future.__await__
Replace the problematic implementation with:

```python
def __await__(self):
    """Make future awaitable"""
    import asyncio
    if not self.done:
        # Create a future that we can await on
        loop = asyncio.get_event_loop()
        future = loop.create_future()
        
        def on_done(fut):
            if not future.done():
                future.set_result(None)
        
        self.add_callback(on_done)
        
        # Wait until the future is set
        yield from future
    # ... rest of implementation remains the same
```

### Fix for Monkey-Patching
Implement a proper event loop policy that returns gsyncio's fiber-based event loop instead of the standard asyncio loop:

```python
class GsyncioEventLoopPolicy(asyncio.DefaultEventLoopPolicy):
    def new_event_loop(self):
        # Return gsyncio's fiber-based event loop implementation
        # This would need to be implemented in gsyncio
        return GsyncioEventLoop()  # Placeholder for actual implementation
```

## Recommendations
1. Address the Critical Issues first to restore basic functionality
2. Implement the monkey-patching properly to deliver on the performance promise
3. Gradually replace asyncio-dependent code in the pure Python fallbacks with fiber-native implementations
4. Consider adding benchmark tests to verify performance improvements over time

These improvements would significantly enhance gsyncio's reliability, performance, and ability to deliver on its promise of high-performance fiber-based concurrency for Python.