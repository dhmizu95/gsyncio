# Performance Improvement Plan v2

## Executive Summary

This plan addresses the critical performance bottlenecks in gsyncio to enable **10 million+ concurrent tasks**:

1. **Sharded Task Counters** - Eliminate atomic contention
2. **Hierarchical Timer Wheel** - O(1) timer operations  
3. **Growing Stacks** - Go-like memory efficiency
4. **Batch Scheduling** - Reduce context switch overhead

---

## Phase 1: Sharded Task Counters

### Problem
Every task spawn/decrement hits the same global atomic counter, causing cache line ping-pong between CPU cores.

### Current Code
```c
// csrc/scheduler.c
uint64_t scheduler_atomic_inc_task_count(void) {
    return __atomic_add_fetch(&g_scheduler->stats.atomic_task_count, 1, __ATOMIC_SEQ_CST);
}
```

### Implementation

#### Step 1.1: Create Sharded Counter Structure
```c
// csrc/scheduler.h
#define NUM_SHARDS 64

typedef struct {
    _Atomic uint64_t counts[NUM_SHARDS];
    _Atomic uint64_t total;  // Updated lazily
} sharded_counter_t;

// csrc/scheduler.c
static sharded_counter_t g_task_counter;
static sharded_counter_t g_completion_counter;

void sharded_counter_init(sharded_counter_t* sc) {
    memset(sc, 0, sizeof(sharded_counter_t));
}

uint64_t sharded_counter_inc(sharded_counter_t* sc, uint32_t worker_id) {
    uint64_t shard = worker_id % NUM_SHARDS;
    return __atomic_add_fetch(&sc->counts[shard], 1, __ATOMIC_RELAXED);
}

uint64_t sharded_counter_get_total(sharded_counter_t* sc) {
    // Use cached total if recent
    uint64_t total = __atomic_load_n(&sc->total, __ATOMIC_ACQUIRE);
    if (total > 0) return total;
    
    // Recalculate
    total = 0;
    for (int i = 0; i < NUM_SHARDS; i++) {
        total += __atomic_load_n(&sc->counts[i], __ATOMIC_RELAXED);
    }
    __atomic_store_n(&sc->total, total, __ATOMIC_RELEASE);
    return total;
}
```

#### Step 1.2: Update Task Registry
```c
// csrc/task.h
typedef struct task_registry {
    sharded_counter_t task_count;
    sharded_counter_t completion_count;
    _Atomic uint64_t next_task_id;
    // ... existing fields
} task_registry_t;
```

#### Step 1.3: Update Spawn Path
```c
// csrc/task.c
uint64_t task_spawn(...) {
    uint32_t worker_id = get_current_worker_id();  // TLS
    scheduler_atomic_inc_task_count_sharded(worker_id);
    // ... rest of spawn logic
}
```

### Expected Impact
- **10-100x** reduction in counter contention
- Enables millions of tasks/second spawn rate

---

## Phase 2: Hierarchical Timer Wheel

### Problem
Current O(n) timer list scan on every tick doesn't scale to millions of timers.

### Current Code
```c
// csrc/evloop.c
evloop_timer_t *timer = loop->timer_list;
while (timer) {
    // O(n) scan!
    timer = timer->next;
}
```

### Implementation

#### Step 2.1: Timer Wheel Structure
```c
// csrc/timer_wheel.h

#define TW_LEVELS 4
#define TW_SLOTS 64

typedef struct timer_node timer_node_t;

typedef struct {
    timer_node_t* head;  // Linked list of timers in this slot
    timer_node_t* tail;
} timer_slot_t;

typedef struct {
    timer_slot_t wheels[TW_LEVELS][TW_SLOTS];
    uint64_t current_time;      // Current tick (ms)
    uint64_t next_expiry;       // Next timer expiry
    
    // Overflow wheel for very long timers
    timer_node_t* overflow;
} timer_wheel_t;

// Initialize
int timer_wheel_init(timer_wheel_t* tw) {
    memset(tw, 0, sizeof(timer_wheel_t));
    return 0;
}

// Add timer - O(1)
void timer_wheel_add(timer_wheel_t* tw, timer_node_t* node, uint64_t delay_ms) {
    uint64_t expiry = tw->current_time + delay_ms;
    node->expiry = expiry;
    
    // Determine which level
    uint64_t ticks = expiry - tw->current_time;
    
    if (ticks < TW_SLOTS) {
        // Level 0: 1ms resolution
        uint64_t slot = expiry % TW_SLOTS;
        list_add_tail(&tw->wheels[0][slot], node);
    } else if (ticks < TW_SLOTS * TW_SLOTS) {
        // Level 1: 64ms resolution  
        uint64_t slot = (expiry / TW_SLOTS) % TW_SLOTS;
        list_add_tail(&tw->wheels[1][slot], node);
    } else if (ticks < TW_SLOTS * TW_SLOTS * TW_SLOTS) {
        // Level 2: 4s resolution
        uint64_t slot = (expiry / (TW_SLOTS * TW_SLOTS)) % TW_SLOTS;
        list_add_tail(&tw->wheels[2][slot], node);
    } else {
        // Overflow - handle separately
        list_add_tail(&tw->overflow, node);
    }
}

// Tick - O(1) amortized
void timer_wheel_tick(timer_wheel_t* tw) {
    uint64_t slot = tw->current_time % TW_SLOTS;
    
    // Process current level 0 slot
    timer_node_t* node = tw->wheels[0][slot].head;
    while (node) {
        timer_node_t* next = node->next;
        // Fire timer
        timer_fire(node);
        node = next;
    }
    tw->wheels[0][slot].head = NULL;
    
    // Every 64 ticks, cascade level 1 -> level 0
    if (slot == 0) {
        // Migrate timers from level 1 to level 0
        // ...
    }
    
    tw->current_time++;
}
```

#### Step 2.2: Integrate with Scheduler
```c
// csrc/scheduler.c
typedef struct scheduler {
    // ... existing fields
    timer_wheel_t timer_wheel;
    
    // Legacy timer support (for backward compatibility)
    evloop_timer_t* timer_list;
    pthread_mutex_t timers_mutex;
} scheduler_t;
```

### Expected Impact
- **O(1)** timer operations instead of O(n)
- Support **millions of concurrent timers**
- Consistent low-latency timer firing

---

## Phase 3: Growing Stacks (Go-like)

### Problem
Each fiber uses fixed 2KB stack = 20GB for 10M fibers.

### Implementation

#### Step 3.1: Smaller Initial Stack
```c
// csrc/fiber.h
#define FIBER_INITIAL_STACK_SIZE 256    // Start with 256 bytes!
#define FIBER_MAX_STACK_SIZE 65536     // 64KB max (like Go)
#define FIBER_STACK_GROW_THRESHOLD 128  // Grow when < 128 bytes free
```

#### Step 3.2: Stack Overflow Detection
```c
// Use SIGSTKSZ and sigaltstack for overflow handling
// When guard page is hit, receive SIGSEGV and grow

static void fiber_handle_overflow(int sig, siginfo_t* info, void* ctx) {
    fiber_t* fiber = g_current_fiber;
    
    // Double the stack size
    size_t new_size = fiber->stack_size * 2;
    if (new_size > FIBER_MAX_STACK_SIZE) {
        // Too large - abort
        fiber->state = FIBER_CANCELLED;
        return;
    }
    
    // Allocate new stack
    void* new_stack = mmap(NULL, new_size + 4096,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    // Copy current stack to new location
    // This is complex - need to save/restore entire stack frame
    memcpy(new_stack, fiber->stack_base, fiber->stack_size);
    
    // Update fiber
    munmap(fiber->stack_base, fiber->stack_capacity + 4096);
    fiber->stack_base = new_stack;
    fiber->stack_ptr = (char*)new_stack + new_size + 4096;
    fiber->stack_size = new_size;
}
```

#### Step 3.3: Alternative - Use makecontext
```c
// Simpler approach using POSIX ucontext
#include <ucontext.h>

typedef struct fiber {
    ucontext_t uc;           // Full context (includes stack info)
    ucontext_t caller_uc;   // Where to return
    // ... other fields
} fiber_t;

// Create fiber with minimal stack
int fiber_create(fiber_t* fiber, void (*func)(void*), void* arg) {
    getcontext(&fiber->uc);
    fiber->uc.uc_stack.ss_size = FIBER_INITIAL_STACK_SIZE;
    fiber->uc.uc_stack.ss_sp = mmap(NULL, FIBER_INITIAL_STACK_SIZE,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fiber->uc.uc_link = &fiber->caller_uc;
    
    makecontext(&fiber->uc, (void(*)(void))func, 1, arg);
}

// Yield
void fiber_yield(fiber_t* fiber) {
    swapcontext(&fiber->uc, &fiber->caller_uc);
}
```

### Expected Impact
- **10M fibers** use ~256MB instead of 20GB
- Memory proportional to actual stack usage

---

## Phase 4: Batch Scheduling

### Problem
Each task spawn is a separate scheduler call with overhead.

### Implementation

#### Step 4.1: Batch Task Structure
```c
// csrc/task.h
typedef struct task_batch {
    void** funcs;           // Array of function pointers
    void** args;           // Array of arguments
    uint32_t count;        // Number of tasks
    uint32_t capacity;     // Allocated capacity
} task_batch_t;
```

#### Step 4.2: Batch Spawn API
```c
// csrc/task.c
int task_spawn_batch(task_batch_t* batch) {
    // Fast path: add all to local queue
    for (uint32_t i = 0; i < batch->count; i++) {
        fiber_t* f = fiber_create(batch->funcs[i], batch->args[i], 0);
        scheduler_ready(f);  // Direct scheduler enqueue
    }
    return 0;
}
```

#### Step 4.3: Python API
```python
# gsyncio/task.py
def task_batch(funcs_and_args):
    """Spawn multiple tasks efficiently"""
    funcs = [f for f, _ in funcs_and_args]
    args = [a for _, a in funcs_and_args]
    return _task_batch(funcs, args)
```

### Expected Impact
- **5-10x** faster bulk task creation
- Better cache locality

---

## Implementation Timeline

| Phase | Description | Effort | Impact |
|-------|-------------|--------|--------|
| 1 | Sharded Counters | 2 days | High |
| 2 | Timer Wheel | 3 days | High |
| 3 | Growing Stacks | 5 days | Medium |
| 4 | Batch Scheduling | 2 days | Medium |

**Total: ~12 days**

---

## Success Metrics

| Metric | Current | Target |
|--------|---------|--------|
| Max concurrent tasks | 500K | 10M+ |
| Task spawn (1K) | 16ms | <5ms |
| Context switch | 1ms | <0.5ms |
| Memory per 1M tasks | ~2GB | <100MB |
| Timer precision | O(n) | O(1) |

---

## Files to Modify

1. `csrc/scheduler.h` - Add sharded counter structures
2. `csrc/scheduler.c` - Implement sharded counters
3. `csrc/timer_wheel.h` - New file
4. `csrc/timer_wheel.c` - New file  
5. `csrc/evloop.c` - Replace timer list with wheel
6. `csrc/fiber.h` - Stack configuration
7. `csrc/fiber.c` - Growing stack implementation
8. `csrc/task.h` - Batch task structures
9. `csrc/task.c` - Batch spawn implementation
10. `gsyncio/task.py` - Python batch API
