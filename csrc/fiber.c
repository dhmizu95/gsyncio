/**
 * fiber.c - Fiber implementation for gsyncio
 * 
 * Stackful coroutines using setjmp/longjmp for context switching.
 * Based on Viper fiber runtime.
 */

#include "fiber.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

/* Global fiber state */
static uint64_t g_fiber_id_counter = 0;

/* Current executing fiber (TLS) */
#ifdef __linux__
__thread fiber_t* g_current_fiber = NULL;
#elif defined(__APPLE__)
__thread fiber_t* g_current_fiber = NULL;
#else
fiber_t* g_current_fiber = NULL;
#endif

/* Scheduler jump buffer for yield return */
static __thread sigjmp_buf g_sched_jump_buf;

/* Global scheduler functions (from scheduler.c) */
extern void* g_scheduler;
extern void scheduler_schedule(fiber_t* f, int worker_id);
extern fiber_t* scheduler_get_ready(void);

/* ============================================ */
/* Signal Handling (for stack growth)          */
/* ============================================ */

static void sigsegv_handler(int sig, siginfo_t* info, void* context) {
    (void)sig;
    (void)context;
    
    /* Find the fiber whose stack was overflowed */
    fiber_t* fiber = g_current_fiber;
    if (!fiber) {
        _exit(1);
    }
    
    /* Check if the fault is in the fiber's stack */
    char* fault_addr = (char*)info->si_addr;
    char* stack_bottom = (char*)fiber->stack_base - fiber->stack_capacity;
    
    if (fault_addr >= stack_bottom && fault_addr < (char*)fiber->stack_base) {
        /* Stack overflow - in a full implementation, we'd grow the stack */
        fprintf(stderr, "Fiber %lu stack overflow\n", (unsigned long)fiber->id);
        _exit(1);
    }
    
    _exit(1);
}

static void setup_signal_handler(void) {
    static int initialized = 0;
    if (initialized) return;
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGSEGV, &sa, NULL);
    initialized = 1;
}

/* ============================================ */
/* Fiber Implementation                         */
/* ============================================ */

int fiber_init(void) {
    setup_signal_handler();
    return 0;
}

void fiber_cleanup(void) {
    g_current_fiber = NULL;
}

fiber_t* fiber_create(void (*func)(void*), void* arg, size_t stack_size) {
    fiber_t* fiber = (fiber_t*)calloc(1, sizeof(fiber_t));
    if (!fiber) {
        return NULL;
    }
    
    fiber->id = __sync_fetch_and_add(&g_fiber_id_counter, 1);
    fiber->state = FIBER_NEW;
    fiber->func = func;
    fiber->arg = arg;
    fiber->parent = g_current_fiber;
    
    /* Set up stack */
    if (stack_size == 0) {
        stack_size = FIBER_DEFAULT_STACK_SIZE;
    }
    fiber->stack_size = stack_size;
    fiber->stack_capacity = stack_size;
    
    /* Allocate stack with guard page */
    fiber->stack_base = mmap(
        NULL,
        stack_size + 4096,  /* Extra page for guard */
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    
    if (fiber->stack_base == MAP_FAILED) {
        free(fiber);
        return NULL;
    }
    
    /* Set up guard page (unreadable/unwritable) */
    mprotect(fiber->stack_base, 4096, PROT_NONE);
    
    /* Stack grows downward, so stack_ptr starts at base + size */
    fiber->stack_ptr = (char*)fiber->stack_base + stack_size + 4096;
    
    return fiber;
}

void fiber_free(fiber_t* fiber) {
    if (!fiber) {
        return;
    }
    
    if (fiber->stack_base && fiber->stack_base != MAP_FAILED) {
        munmap(fiber->stack_base, fiber->stack_capacity + 4096);
    }
    
    free(fiber);
}

uint64_t fiber_id(fiber_t* fiber) {
    return fiber ? fiber->id : 0;
}

fiber_state_t fiber_state(fiber_t* fiber) {
    return fiber ? fiber->state : FIBER_CANCELLED;
}

fiber_t* fiber_current(void) {
    return g_current_fiber;
}

int fiber_start(fiber_t* fiber) {
    if (!fiber || fiber->state != FIBER_NEW) {
        return -1;
    }
    
    fiber->state = FIBER_READY;
    
    /* Add to scheduler */
    if (g_scheduler) {
        scheduler_schedule(fiber, -1);
    }
    
    return 0;
}

void fiber_yield(void) {
    fiber_t* current = g_current_fiber;
    if (!current) {
        return;
    }
    
    /* Mark as ready (not waiting - we want to run again) */
    current->state = FIBER_READY;
    
    /* Add back to scheduler queue */
    if (g_scheduler) {
        scheduler_schedule(current, -1);
    }
    
    /* Set up jump point for return */
    current->sched_jump = &g_sched_jump_buf;
    
    /* Save context and jump to scheduler */
    if (sigsetjmp(g_sched_jump_buf, 1) == 0) {
        /* First time - jump to scheduler to get next fiber */
        /* The scheduler will handle picking the next fiber */
    }
    /* else: we returned here via siglongjmp - continue execution */
    
    current->sched_jump = NULL;
}

void fiber_resume(fiber_t* fiber) {
    if (!fiber) {
        return;
    }
    
    fiber->state = FIBER_READY;
    
    if (g_scheduler) {
        scheduler_schedule(fiber, -1);
    }
}

void fiber_switch(fiber_t* from, fiber_t* to) {
    if (!from || !to) {
        return;
    }
    
    /* Save current fiber context */
    if (sigsetjmp(from->context, 1) == 0) {
        /* First time - switch to 'to' fiber */
        
        /* Update current fiber pointer */
        g_current_fiber = to;
        to->state = FIBER_RUNNING;
        
        /* Set up jump point for 'to' fiber */
        to->sched_jump = &g_sched_jump_buf;
        
        /* Jump to the 'to' fiber's context */
        siglongjmp(to->context, 1);
    }
    /* else: we returned here from 'to' fiber yielding/switching back */
}

void fiber_park(void) {
    fiber_t* fiber = g_current_fiber;
    if (!fiber) {
        return;
    }
    
    /* Mark as waiting/parked */
    fiber->state = FIBER_WAITING;
    
    /* Yield to scheduler */
    fiber_yield();
}

void fiber_unpark(fiber_t* fiber) {
    if (!fiber) {
        return;
    }
    
    /* Mark as ready */
    fiber->state = FIBER_READY;
    
    /* Add back to scheduler */
    if (g_scheduler) {
        scheduler_schedule(fiber, -1);
    }
}

bool fiber_is_parked(fiber_t* fiber) {
    if (!fiber) {
        return false;
    }
    return fiber->state == FIBER_WAITING;
}
