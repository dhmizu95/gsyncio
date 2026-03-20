# Fiber Pool Counter Inc/Dec Analysis and Fix Plan

## Executive Summary

Analysis of the fiber pool counter operations in [`csrc/fiber_pool.c`](csrc/fiber_pool.c) has revealed **critical bugs** in the allocation and deallocation paths that can cause crashes, memory corruption, and counter inconsistency.

---

## Identified Issues

### Issue 1: CRITICAL - NULL Pool Pointer Access (Line 213)

**Location:** [`fiber_pool_alloc()`](csrc/fiber_pool.c:213)

**Problem:**
```c
// Line 174: memset clears the ENTIRE fiber struct including pool pointer
memset(fiber, 0, sizeof(fiber_t));

// ... stack restoration code ...

// Line 213: Uses pool pointer that was just zeroed!
fiber->id = atomic_fetch_add(&pool->allocated, 1) + 1;

// Line 214: Pool is set AFTER it's used!
fiber->pool = pool;
```

The `memset(fiber, 0, sizeof(fiber_t))` at line 174 clears the `pool` pointer, but line 213 dereferences `pool` before it's restored at line 214. This is a **use-after-free bug** (use of NULL pointer) that causes crashes.

**Impact:** Memory corruption, crashes, undefined behavior.

---

### Issue 2: Counter Operations Not Atomic Together

**Location:** [`fiber_pool_alloc()`](csrc/fiber_pool.c:213-219) and [`fiber_pool_free()`](csrc/fiber_pool.c:349-350)

**Problem:**
```c
// In fiber_pool_alloc:
fiber->id = atomic_fetch_add(&pool->allocated, 1) + 1;  // Step 1
// ... 
atomic_fetch_sub(&pool->available, 1);  // Step 2

// In fiber_pool_free:
atomic_fetch_add(&pool->available, 1);   // Step 1
atomic_fetch_sub(&pool->allocated, 1);   // Step 2
```

The two counter operations are not atomic together, creating a window where `available + allocated != capacity`.

**Impact:** Counter inconsistency, potential misreporting of pool state.

---

### Issue 3: Non-Atomic Initialization

**Location:** [`fiber_pool_create()`](csrc/fiber_pool.c:124-125)

**Problem:**
```c
pool->available = pool->capacity;  // Non-atomic write
pool->allocated = 0;               // Non-atomic write
```

These are plain assignments, not atomic operations. With lock-free consumers, there's no visibility guarantee across threads.

**Impact:** Potential visibility issues in multi-threaded environments.

---

### Issue 4: Stack Allocation Failure Handling Race

**Location:** [`fiber_pool_alloc()`](csrc/fiber_pool.c:194-199)

**Problem:**
```c
if (new_stack == MAP_FAILED) {
    // Put fiber back to free list before returning NULL
    atomic_store(&head->next, atomic_load(&pool->free_list));
    atomic_store(&pool->free_list, head);
    return NULL;
}
```

This recovery code is not atomic with the earlier CAS that removed the node from the free list. Another thread could have modified the free list in between.

**Impact:** Potential free list corruption, lost fibers.

---

### Issue 5: No Counter Consistency Verification

**Problem:** There's no debug/assertion check to verify that `available + allocated == capacity` after each operation.

**Impact:** Hard to detect counter corruption in production.

---

## Fix Plan

### Fix 1: Save Pool Pointer Before Memset (CRITICAL)

**Priority:** P0 - Critical

**Solution:**
Save the pool pointer BEFORE the memset, then restore it immediately after:

```c
fiber_t* fiber_pool_alloc(fiber_pool_t* pool) {
    // ... existing code ...
    
    // Save pool pointer BEFORE memset
    void* saved_pool = fiber->pool;
    
    // Save stack info before reset
    void* saved_stack_base = fiber->stack_base;
    size_t saved_stack_capacity = fiber->stack_capacity;
    char* saved_stack_ptr = fiber->stack_ptr;
    
    // Reset fiber state
    memset(fiber, 0, sizeof(fiber_t));
    
    // Restore pool pointer IMMEDIATELY after memset
    fiber->pool = saved_pool;
    
    // ... rest of stack handling ...
    
    // Now safe to use pool
    fiber->id = atomic_fetch_add(&pool->allocated, 1) + 1;
    fiber->state = FIBER_NEW;
    
    atomic_fetch_sub(&pool->available, 1);
    
    return fiber;
}
```

---

### Fix 2: Combine Counter Operations with Double-Width CAS or Single Atomic Update

**Priority:** P1 - High

**Solution Option A:** Use a single combined counter for "in-use" fibers:
```c
// Instead of two separate counters, use one
_Atomic size_t in_use;  // fibers currently allocated

// On alloc:
atomic_fetch_add(&pool->in_use, 1);

// On free:  
atomic_fetch_sub(&pool->in_use, 1);

// available = capacity - in_use
```

**Solution Option B:** Use a mutex for counter updates (simpler, less performant but correct):
```c
// In fiber_pool_alloc:
pthread_mutex_lock(&pool->counter_mutex);
pool->allocated++;
pool->available--;
pthread_mutex_unlock(&pool->counter_mutex);
```

**Recommendation:** Use Solution A (single counter) for best performance.

---

### Fix 3: Use Atomic Initialization

**Priority:** P1 - High

**Solution:**
```c
// In fiber_pool_create:
atomic_store(&pool->available, pool->capacity);
atomic_store(&pool->allocated, 0);
```

---

### Fix 4: Fix Stack Allocation Failure Handling

**Priority:** P2 - Medium

**Solution:** Use a more robust recovery mechanism or simplify by not putting the fiber back (let it leak - rare failure case):

```c
if (new_stack == MAP_FAILED) {
    // Just leak this fiber node - it's extremely rare
    // The pool will grow dynamically if needed
    DEBUG_LOG("Failed to allocate stack for fiber, leaking node");
    return NULL;
}
```

Or better: Don't remove from free list until after stack allocation succeeds. Restructure the code to:

1. Check if stack is needed
2. Allocate stack first
3. Then remove from free list

---

### Fix 5: Add Counter Consistency Debug Check

**Priority:** P2 - Medium

**Solution:** Add assertion in debug builds:
```c
void fiber_pool_verify_counters(fiber_pool_t* pool) {
    size_t avail = atomic_load(&pool->available);
    size_t alloc = atomic_load(&pool->allocated);
    assert(avail + alloc == pool->capacity);
}
```

---

## Implementation Checklist

- [ ] **Fix 1:** Save pool pointer before memset, restore immediately after
- [ ] **Fix 2:** Use single `in_use` counter instead of separate allocated/available
- [ ] **Fix 3:** Use atomic_store for initialization
- [ ] **Fix 4:** Improve stack allocation failure handling
- [ ] **Fix 5:** Add counter verification debug function

---

## Files to Modify

1. [`csrc/fiber_pool.c`](csrc/fiber_pool.c) - Main fixes
2. [`csrc/fiber_pool.h`](csrc/fiber_pool.h) - May need counter verification function

---

## Testing Plan

1. **Unit Test:** Create test that spawns and completes 10,000 tasks rapidly
2. **Counter Test:** Verify `available + allocated == capacity` after each operation
3. **Stress Test:** Run with GSYNCIO_DEBUG=1 to catch any issues
4. **Memory Test:** Run valgrind or ASan to detect memory issues
