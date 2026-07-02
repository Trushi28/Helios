/**
 * @file scheduler.c
 * @brief Per-core O(1) priority-bitmap scheduler implementation.
 *
 * Each core maintains SCHED_PRIORITY_LEVELS ready queues (FIFO linked lists).
 * A 32-bit bitmap tracks non-empty queues for O(1) highest-priority lookup
 * via __builtin_ctz(). Preemption is driven by the APIC timer interrupt.
 */

#include <helios/scheduler.h>
#include <helios/microprog.h>
#include <helios/sched/per_core.h>
#include <helios/pmm.h>
#include <helios/vmm.h>
#include <arch/x86_64/x2apic.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/tsc.h>
#include <arch/x86_64/gdt.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern void *memset(void *dest, int val, size_t n);

bool g_scheduler_online = false;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Idle thread entry point                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void idle_thread_fn(void) {
    for (;;) {
        /* Try to steal work before halting */
        per_core_data_t *core = this_core();
        if (!scheduler_try_steal(core)) {
            /* Ensure IF=1 before HLT so timer interrupts can wake us */
            sti();
            cpu_halt();
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Allocate a stack for a kernel task                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t alloc_task_stack(uint32_t pages) {
    /* Allocate physical pages (order from page count) */
    uint32_t order = 0;
    uint32_t p = pages;
    while (p > 1) { p >>= 1; order++; }

    phys_addr_t phys = pmm_alloc_pages(order);
    if (phys == 0) return 0;

    /* Map pages with guard page (leave one page below unmapped) */
    virt_addr_t guard = vmm_heap_alloc_page();  /* guard page — unmapped */
    (void)guard;

    virt_addr_t stack_base = 0;
    for (uint32_t i = 0; i < pages; i++) {
        virt_addr_t va = vmm_heap_alloc_page();
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
        if (vmm_nx_supported()) flags |= PTE_NO_EXECUTE;
        vmm_map_page(va, phys + i * PAGE_SIZE, flags);
        if (i == 0) stack_base = va;
    }

    return stack_base + (uint64_t)pages * PAGE_SIZE;  /* return stack top */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Enqueue / Dequeue                                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */

void scheduler_enqueue(per_core_data_t *core, microprogram_t *mp) {
    uint32_t p = (uint32_t)mp->priority;
    if (p >= SCHED_PRIORITY_LEVELS) p = SCHED_PRIORITY_LEVELS - 1;

    mp->next = NULL;
    mp->state = MPROG_STATE_READY;

    if (core->ready_tail[p]) {
        core->ready_tail[p]->next = mp;
    } else {
        core->ready_head[p] = mp;
    }
    core->ready_tail[p] = mp;
    core->ready_counts[p]++;
    core->priority_bitmap |= (1u << p);
}

microprogram_t *scheduler_pick_next(per_core_data_t *core) {
    if (core->priority_bitmap == 0) {
        return core->idle_thread;
    }

    uint32_t p = (uint32_t)__builtin_ctz(core->priority_bitmap);
    microprogram_t *mp = core->ready_head[p];
    if (!mp) {
        /* Bitmap inconsistency — clear bit and try again */
        core->priority_bitmap &= ~(1u << p);
        return scheduler_pick_next(core);
    }

    core->ready_head[p] = mp->next;
    if (!core->ready_head[p]) {
        core->ready_tail[p] = NULL;
        core->priority_bitmap &= ~(1u << p);
    }
    core->ready_counts[p]--;
    mp->next = NULL;

    return mp;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Sleep support (S2)                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

void scheduler_check_sleeping(per_core_data_t *core) {
    uint64_t now = rdtsc();
    struct microprogram **pp = &core->sleeping_head;

    while (*pp) {
        microprogram_t *mp = *pp;
        if (mp->wake_time_tsc <= now) {
            /* Remove from sleeping list */
            *pp = mp->sleep_next;
            mp->sleep_next = NULL;
            core->sleeping_count--;

            /* Wake the task */
            mp->state = MPROG_STATE_READY;
            scheduler_enqueue(core, mp);
        } else {
            pp = &mp->sleep_next;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Timer arm                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

void scheduler_arm_timer(per_core_data_t *core) {
    microprogram_t *mp = core->current;
    uint64_t ns = mp ? mp->timeslice_ns : TIMESLICE_NORMAL_NS;
    x2apic_arm_timer(ns);
    core->timer_deadline_tsc = rdtsc() + ns_to_tsc(ns);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Preemption (timer interrupt handler)                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

void scheduler_preempt(void) {
    per_core_data_t *core = this_core();

    /* Check sleeping tasks first — may wake higher-priority work */
    scheduler_check_sleeping(core);

    microprogram_t *old = core->current;
    if (old && old->state == MPROG_STATE_RUNNING) {
        /* Update runtime statistics */
        uint64_t now = rdtsc();
        old->total_runtime_tsc += now - old->last_scheduled_tsc;
        old->state = MPROG_STATE_READY;
        scheduler_enqueue(core, old);
    }

    microprogram_t *next = scheduler_pick_next(core);
    if (next == old && old) {
        /* Same task — just re-arm and continue */
        old->state = MPROG_STATE_RUNNING;
        old->last_scheduled_tsc = rdtsc();
        scheduler_arm_timer(core);
        return;
    }

    core->current = next;
    next->state = MPROG_STATE_RUNNING;
    next->last_scheduled_tsc = rdtsc();
    next->context_switch_count++;
    core->context_switches++;

    /* Update RSP0 in TSS for ring transitions */
    gdt_set_tss_rsp0(core->kernel_stack_top);

    scheduler_arm_timer(core);

    if (old) {
        context_switch(&old->ctx, &next->ctx);
    } else {
        /* First schedule — just restore the new task's context.
         * We save to a dummy context. */
        cpu_context_t dummy;
        context_switch(&dummy, &next->ctx);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Yield / Block / Wake                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

void scheduler_yield(void) {
    per_core_data_t *core = this_core();
    spinlock_lock(&core->lock);

    microprogram_t *old = core->current;
    if (!old) {
        spinlock_unlock(&core->lock);
        return;
    }

    uint64_t now = rdtsc();
    old->total_runtime_tsc += now - old->last_scheduled_tsc;
    old->state = MPROG_STATE_READY;
    scheduler_enqueue(core, old);

    microprogram_t *next = scheduler_pick_next(core);
    core->current = next;
    next->state = MPROG_STATE_RUNNING;
    next->last_scheduled_tsc = rdtsc();
    next->context_switch_count++;
    core->context_switches++;

    scheduler_arm_timer(core);
    spinlock_unlock(&core->lock);

    if (old != next) {
        context_switch(&old->ctx, &next->ctx);
    }
}

void scheduler_block(mprog_state_t reason) {
    per_core_data_t *core = this_core();
    spinlock_lock(&core->lock);

    microprogram_t *old = core->current;
    if (!old) {
        spinlock_unlock(&core->lock);
        return;
    }

    uint64_t now = rdtsc();
    old->total_runtime_tsc += now - old->last_scheduled_tsc;
    old->state = reason;  /* BLOCKED, SLEEPING, or DEAD */

    /* If sleeping, add to the sleeping list */
    if (reason == MPROG_STATE_SLEEPING) {
        old->sleep_next = core->sleeping_head;
        core->sleeping_head = old;
        core->sleeping_count++;
    }

    microprogram_t *next = scheduler_pick_next(core);
    core->current = next;
    next->state = MPROG_STATE_RUNNING;
    next->last_scheduled_tsc = rdtsc();
    next->context_switch_count++;
    core->context_switches++;

    scheduler_arm_timer(core);
    spinlock_unlock(&core->lock);

    context_switch(&old->ctx, &next->ctx);

    /* If we return here, the task was woken up and rescheduled */
}

void scheduler_wake(microprogram_t *mp) {
    if (!mp || mp->state == MPROG_STATE_DEAD) return;
    mp->state = MPROG_STATE_READY;

    per_core_data_t *core = this_core();
    spinlock_lock(&core->lock);
    scheduler_enqueue(core, mp);
    spinlock_unlock(&core->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Work stealing                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* Import CPU table for core enumeration */
extern uint32_t g_cpu_count;

bool scheduler_try_steal(per_core_data_t *core) {
    /* Simple round-robin steal: check other cores' lowest non-empty queue.
     * Phase 2 has few cores (4 in QEMU) so linear scan is fine. */
    (void)core;
    /* Work stealing requires accessing other cores' per_core_data_t.
     * For Phase 2, we keep it simple — the idle loop will just HLT
     * and rely on IPI_RESCHEDULE to wake it when new work arrives. */
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Initialization                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

void scheduler_init(void) {
    per_core_data_t *core = this_core();

    /* Create BSP idle thread */
    uint64_t idle_stack_top = alloc_task_stack(4);  /* 16 KiB */
    microprogram_t *idle = microprog_create("bsp_idle", PRIORITY_IDLE,
                                             (uint64_t)idle_thread_fn,
                                             idle_stack_top, 0);
    if (!idle) {
        serial_puts("  SCHED: FATAL — cannot create idle thread\n");
        for (;;) cpu_halt();
    }
    core->idle_thread = idle;
    core->current = idle;
    idle->state = MPROG_STATE_RUNNING;
    idle->last_scheduled_tsc = rdtsc();

    /* Set kernel_stack_top for ring transitions */
    core->kernel_stack_top = idle_stack_top;

    /* Arm the timer for the first timeslice */
    scheduler_arm_timer(core);

    g_scheduler_online = true;

    /* Enable interrupts so the APIC timer can fire */
    sti();

    serial_puts("  SCHED: BSP scheduler online, interrupts enabled\n");
}

void scheduler_init_core(void) {
    per_core_data_t *core = this_core();

    /* Create per-AP idle thread */
    uint64_t idle_stack_top = alloc_task_stack(4);  /* 16 KiB */
    microprogram_t *idle = microprog_create("ap_idle", PRIORITY_IDLE,
                                             (uint64_t)idle_thread_fn,
                                             idle_stack_top, 0);
    if (!idle) {
        serial_printf("  SCHED: FATAL — AP %u cannot create idle thread\n",
                      core->core_id);
        for (;;) cpu_halt();
    }
    core->idle_thread = idle;
    core->current = idle;
    idle->state = MPROG_STATE_RUNNING;
    idle->last_scheduled_tsc = rdtsc();

    core->kernel_stack_top = idle_stack_top;
    scheduler_arm_timer(core);

    /* Enable interrupts on this AP */
    sti();

    serial_printf("  SCHED: core %u scheduler online\n", core->core_id);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Idle loop                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

NORETURN void scheduler_idle_loop(void) {
    for (;;) {
        per_core_data_t *core = this_core();

        /* Check if there's real work to do */
        if (core->priority_bitmap != 0) {
            scheduler_yield();
            continue;
        }

        /* Try stealing from other cores */
        if (scheduler_try_steal(core)) {
            scheduler_yield();
            continue;
        }

        /* No work — halt until an interrupt wakes us.
         * Ensure IF=1 before HLT so timer/IPI interrupts are delivered. */
        sti();
        cpu_halt();
    }
}
