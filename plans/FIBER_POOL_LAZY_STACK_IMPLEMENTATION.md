# FIBER_POOL_LAZY_STACK Implementation Plan

## Problem Statement
The current `FIBER_POOL_LAZY_STACK=1` setting does not actually allocate stacks lazily - it just leaves fibers with NULL stacks, causing crashes. The fiber pool code assumes pre-allocated stacks but they're not being created.

## Current Behavior (Broken)
- `FIBER_POOL_LAZY_STACK=1`: Fibers allocated with NULL stack pointers
- `FIBER_POOL_LAZY_STACK=0`: Fibers pre-allocate stacks (works but uses more memory)

## Target Behavior
- Stacks allocated on-demand when fiber first executes
- Reduces initial memory footprint
- Maintains performance for high-concurrency workloads

## Implementation Plan

### Phase 1: Add Lazy Stack Allocation to fiber_pool_alloc
**Location**: `csrc/fiber_pool.c`

1. **Modify fiber_pool_alloc to allocate stack on-demand**
   - When FIBER_POOL_LAZY_STACK=1, allocate stack during first allocation
   - Use mmap with guard pages similar to fiber_create()

```c
// In fiber_pool_alloc(), after memset:
#if FIBER_POOL_LAZY_STACK == 1
if (!fiber->stack_base) {
    fiber->stack_base = mmap(NULL, FIBER_DEFAULT_STACK_SIZE + 4096,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fiber->stack_base != MAP_FAILED) {
        mprotect(fiber->stack_base, 4096, PROT_NONE);
        fiber->stack_capacity = FIBER_DEFAULT_STACK_SIZE;
        fiber->stack_ptr = (char*)fiber->stack_base + FIBER_DEFAULT_STACK_SIZE + 4096;
    }
}
#endif
```

### Phase 2: Add Lazy Stack Allocation to fiber_pool_create
**Location**: `csrc/fiber_pool.c`

2. **Modify fiber_pool_create to NOT pre-allocate stacks**
   - Remove stack allocation in pool initialization
   - Only allocate fiber control blocks

### Phase 3: Handle Stack Growth (Optional)
**Location**: `csrc/fiber.c`

3. **Implement signal handler for stack overflow**
   - Currently exists but basic - expand to grow stack on SIGSEGV

### Phase 4: Testing
4. **Verify memory usage is lower with lazy allocation**
5. **Test performance with 10K+ tasks**
6. **Test stack overflow handling**

## Configuration Options
```c
#define FIBER_POOL_LAZY_STACK 1     // Enable lazy allocation
#define FIBER_DEFAULT_STACK_SIZE 4096 // 4KB default
#define FIBER_MAX_STACK_SIZE 65536   // 64KB max
#define FIBER_STACK_GROW_STEP 4096   // Grow by 4KB
```

## Memory Tradeoffs
| Setting | Initial Memory (8K fibers) | Per-Fiber Overhead |
|---------|---------------------------|-------------------|
| Eager (0) | ~40MB (8K × 5KB) | 5KB |
| Lazy (1) | ~1MB (just control blocks) | 0KB initially, 5KB on first use |

## Implementation Checklist
- [ ] Modify fiber_pool_create to skip stack allocation
- [ ] Modify fiber_pool_alloc to allocate stack on first use
- [ ] Test with FIBER_POOL_LAZY_STACK=1
- [ ] Verify memory usage reduction
- [ ] Test with 10K+ concurrent tasks
- [ ] Document behavior

## Files Modified
| File | Changes |
|------|---------|
| `csrc/fiber_pool.c` | Add lazy stack allocation logic |
| `csrc/fiber.h` | Update stack size defaults if needed |
