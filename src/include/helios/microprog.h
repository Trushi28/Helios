/**
 * @file microprog.h
 * @brief Microprogram (task) control block — the schedulable unit in Helios.
 *
 * Each microprogram has its own kernel stack, CPU context (callee-saved
 * registers only for Phase 2 kernel-mode tasks), priority, and scheduler
 * linkage. FXSAVE/SSE state is deferred to Phase 3 (ring-3 user tasks)
 * since Phase 2 builds with -mno-sse.
 */

#ifndef HELIOS_MICROPROG_H
#define HELIOS_MICROPROG_H

#include <helios/types.h>

/* Forward declaration — breaks circular dependency with per_core.h */
struct per_core_data;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  CPU context — callee-saved registers only                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Context saved/restored by context_switch() (context.asm).
 * Only callee-saved registers + RSP are stored — the compiler
 * already saves caller-saved registers around function calls.
 * Order must match the push/pop sequence in context.asm:
 *   push r15, r14, r13, r12, rbx, rbp; save rsp
 *   restore: load rsp; pop rbp, rbx, r12, r13, r14, r15; ret
 */
typedef struct {
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} cpu_context_t;

_Static_assert(sizeof(cpu_context_t) == 56,
               "cpu_context_t must be 7 x 8 = 56 bytes");

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  State and priority enums                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    MPROG_STATE_READY    = 0,
    MPROG_STATE_RUNNING  = 1,
    MPROG_STATE_BLOCKED  = 2,
    MPROG_STATE_SLEEPING = 3,
    MPROG_STATE_DEAD     = 4,
} mprog_state_t;

typedef enum {
    PRIORITY_REALTIME = 0,   /* highest — ISR bottom halves    */
    PRIORITY_HIGH     = 1,
    PRIORITY_NORMAL   = 2,   /* default                        */
    PRIORITY_LOW      = 3,
    PRIORITY_IDLE     = 4,   /* lowest — idle threads only     */
} priority_t;

/* Timeslice in nanoseconds per priority level */
#define TIMESLICE_REALTIME_NS  ( 1 * 1000000ULL)  /*  1 ms */
#define TIMESLICE_HIGH_NS      ( 4 * 1000000ULL)  /*  4 ms */
#define TIMESLICE_NORMAL_NS    (10 * 1000000ULL)  /* 10 ms */
#define TIMESLICE_LOW_NS       (20 * 1000000ULL)  /* 20 ms */
#define TIMESLICE_IDLE_NS      (50 * 1000000ULL)  /* 50 ms */

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Microprogram control block                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define MPROG_NAME_MAX 64

typedef struct microprogram {
    /* ── Identity ─────────────────────────────────────────────────────── */
    uint32_t       id;
    char           name[MPROG_NAME_MAX];

    /* ── State and scheduling ─────────────────────────────────────────── */
    mprog_state_t  state;
    priority_t     priority;
    cpu_context_t  ctx;

    /* ── Scheduler linkage (intrusive linked list) ────────────────────── */
    struct microprogram *next;       /* ready-queue / free-list link     */

    /* ── Sleep support (S2) ───────────────────────────────────────────── */
    uint64_t       wake_time_tsc;    /* absolute TSC tick to wake at     */
    struct microprogram *sleep_next; /* sleeping-list link               */

    /* ── Affinity and stack ───────────────────────────────────────────── */
    uint32_t       affinity_core;    /* preferred core (0xFFFFFFFF = any) */
    uint64_t       stack_base;       /* bottom of allocated stack        */
    uint32_t       stack_pages;      /* number of stack pages            */

    /* ── Statistics ────────────────────────────────────────────────────── */
    uint64_t       total_runtime_tsc;
    uint64_t       last_scheduled_tsc;
    uint64_t       timeslice_ns;
    uint32_t       context_switch_count;
} microprogram_t;

_Static_assert(sizeof(microprogram_t) <= 1024,
               "microprogram_t must fit in the 1024-byte slab cache");

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  API                                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a new microprogram.
 *
 * Allocates from the "microprogram" slab cache, sets up the CPU context
 * so that context_switch() will begin execution at entry_rip.
 *
 * @param name       Human-readable name.
 * @param priority   Scheduling priority.
 * @param entry_rip  Function pointer for the task's entry point.
 * @param stack_top  Top of the pre-allocated stack.
 * @param arg        Argument passed in RDI on first context switch.
 * @return Pointer to the new microprogram, or NULL on failure.
 */
microprogram_t *microprog_create(const char *name, priority_t priority,
                                  uint64_t entry_rip, uint64_t stack_top,
                                  uint64_t arg);

/**
 * @brief Destroy a microprogram and free its resources.
 */
void microprog_destroy(microprogram_t *mp);

/**
 * Hardcoded offset of per_core_data_t.current — must match the struct
 * layout in per_core.h. Verified by _Static_assert in per_core.c.
 * Layout: self(8) + kernel_stack_top(8) + user_rsp_save(8) +
 *         core_id(4) + x2apic_id(4) = 32
 */
#define PER_CORE_CURRENT_OFFSET 32

/**
 * @brief Get the currently running microprogram on this core.
 */
static ALWAYS_INLINE microprogram_t *current_microprog(void) {
    microprogram_t *mp;
    __asm__ volatile(
        "movq %%gs:0, %%rax\n\t"                      /* rax = this_core() */
        "movq %c1(%%rax), %0"                          /* mp = core->current */
        : "=r"(mp)
        : "i"(PER_CORE_CURRENT_OFFSET)
        : "rax"
    );
    return mp;
}

/**
 * @brief Low-level context switch (implemented in context.asm).
 *
 * Saves callee-saved registers to save_to, restores from restore_from.
 * On the first switch to a new task, execution begins at the entry_rip
 * that was pre-populated on the stack by microprog_create().
 */
extern void context_switch(cpu_context_t *save_to, cpu_context_t *restore_from);

#endif /* HELIOS_MICROPROG_H */
