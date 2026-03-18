# Task Spawn Performance Improvement Plan

## Current Performance
- Current: 13K tasks/second (68.94ms for 1000 tasks)
- Target: 397K/s (14x improvement) or 187K/s (14x improvement) - need clarification on scenarios
- Go reference: 0.53ms for 1000 tasks (~1.9M/s)

## Root Causes (from existing analysis)
1. **Python wrapper overhead**: Each task creates a wrapper function with try/except
2. **Lock contention**: Global lock for task counting
3. **Individual spawning**: One Python→C call per task
4. **Object allocation**: New wrapper objects for each task

## Improvement Strategy

### Phase 1: Quick Wins (2-5x improvement)
1. **Eliminate exception handling in hot path**
   - Move try/except to debug-only mode
   - Estimated: 2x improvement

2. **Object pooling for task payloads**
   - Pre-allocate TaskPayload objects
   - Reuse instead of creating new ones
   - Estimated: 2x improvement

3. **Batch spawning API**
   - Add `spawn_batch(funcs_and_args)` to spawn multiple tasks in one call
   - Reduces Python loop overhead
   - Estimated: 5x for bulk operations

### Phase 2: C-Level Optimizations (3-10x improvement)
1. **Direct C task spawning without Python wrappers**
   - Store func/args in C struct, call directly from C
   - Eliminate Python wrapper function calls
   - Estimated: 3x improvement

2. **Lock-free task counting**
   - Use atomic operations instead of mutex
   - Reduce lock contention
   - Estimated: 2x improvement

3. **Per-worker task queues**
   - Distribute tasks directly to workers
   - Reduce central queue contention
   - Estimated: 3x improvement

### Phase 3: Advanced Optimizations (2-5x improvement)
1. **Inline caching for function calls**
   - Cache function pointers to avoid GIL overhead
   - Estimated: 2x for C extensions

2. **CPU pinning and NUMA awareness**
   - Optimize memory locality
   - Estimated: 1.5x improvement

3. **Speculative fiber allocation**
   - Pre-allocate fibers before needed
   - Estimated: 1.2x improvement

## Implementation Roadmap

### Week 1: Phase 1 Implementation
- [ ] Implement object pooling
- [ ] Add batch spawning API
- [ ] Conditional exception handling
- [ ] Benchmark improvements

### Week 2: Phase 2 Implementation
- [ ] Direct C spawning
- [ ] Atomic task counting
- [ ] Per-worker queues
- [ ] Integration testing

### Week 3: Phase 3 and Tuning
- [ ] Inline caching
- [ ] CPU pinning
- [ ] Performance profiling and tuning

## Expected Outcomes
- Phase 1: 10-25x improvement (130K-325K/s)
- Phase 2: 30-250x improvement (390K-3.25M/s)
- Phase 3: 60-1250x improvement (780K-16.25M/s)

## Validation
- Maintain correctness with existing tests
- Benchmark against Go implementation
- Profile with perf tools
- Memory usage monitoring

## Dependencies
- Access to C code modifications
- Benchmarking infrastructure
- Profiling tools (perf, valgrind)