/**
 * @file per_core.h
 * @brief Per-core data structure accessed via GS segment base.
 *
 * CRITICAL LAYOUT CONSTRAINT:
 *   The first three fields (self, kernel_stack_top, user_rsp_save) are
 *   at fixed offsets 0, 8, and 16 respectively. The SYSCALL entry stub
 *   (syscall_entry.asm) hardcodes [gs:0], [gs:8], and [gs:16] to access
 *   them. These three fields MUST remain first in the struct, in this
 *   exact order, with NO preceding fields or padding. Any change that
 *   shifts them will silently corrupt register saves and crash.
 */

#ifndef HELIOS_SCHED_PER_CORE_H
#define HELIOS_SCHED_PER_CORE_H

#include <helios/types.h>
#include <helios/spinlock.h>

/* Forward declaration — breaks circular dependency with microprog.h */
struct microprogram;

/* Number of scheduler priority levels */
#define SCHED_PRIORITY_LEVELS 5

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Per-core data structure                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct per_core_data {
    /* ── Fixed-layout fields (assembly-hardcoded offsets) ──────────────── */
    struct per_core_data *self;           /* offset  0: self pointer [gs:0]  */
    uint64_t              kernel_stack_top; /* offset  8: [gs:8]              */
    uint64_t              user_rsp_save;   /* offset 16: [gs:16]             */

    /* ── General identification ────────────────────────────────────────── */
    uint32_t core_id;
    uint32_t x2apic_id;

    /* ── Scheduler state ──────────────────────────────────────────────── */
    struct microprogram *current;                      /* running task     */
    struct microprogram *ready_head[SCHED_PRIORITY_LEVELS];
    struct microprogram *ready_tail[SCHED_PRIORITY_LEVELS];
    uint32_t             ready_counts[SCHED_PRIORITY_LEVELS];
    uint32_t             priority_bitmap;              /* O(1) lookup      */
    struct microprogram *idle_thread;

    /* Timer */
    uint64_t timer_deadline_tsc;

    /* Sleep support (S2) */
    struct microprogram *sleeping_head;   /* linked list of sleeping tasks */
    uint32_t             sleeping_count;

    /* Statistics */
    uint64_t context_switches;
    uint64_t steals;

    /* Per-core scheduler lock */
    spinlock_t lock;
} per_core_data_t;

/* Verify fixed-layout offsets at compile time */
_Static_assert(__builtin_offsetof(per_core_data_t, self) == 0,
               "per_core_data_t.self must be at offset 0");
_Static_assert(__builtin_offsetof(per_core_data_t, kernel_stack_top) == 8,
               "per_core_data_t.kernel_stack_top must be at offset 8");
_Static_assert(__builtin_offsetof(per_core_data_t, user_rsp_save) == 16,
               "per_core_data_t.user_rsp_save must be at offset 16");

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  API                                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Get a pointer to the current core's per_core_data_t.
 * Reads from [gs:0] which points to self.
 */
static ALWAYS_INLINE per_core_data_t *this_core(void) {
    per_core_data_t *p;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(p));
    return p;
}

/**
 * @brief Allocate and initialize a per_core_data_t for a core.
 * @param core_id    Logical core index.
 * @param x2apic_id  APIC ID of the core.
 * @return Pointer to the initialized per_core_data_t.
 */
per_core_data_t *per_core_init(uint32_t core_id, uint32_t x2apic_id);

/**
 * @brief Set the GS base MSRs to point to the per_core_data_t.
 * Sets both GS_BASE and KERNEL_GS_BASE so swapgs works correctly.
 */
void per_core_set_gsbase(per_core_data_t *data);

#endif /* HELIOS_SCHED_PER_CORE_H */
