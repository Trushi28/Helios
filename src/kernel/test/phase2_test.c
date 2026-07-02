/**
 * @file phase2_test.c
 * @brief Phase 2 test micro-programs — ring-0 tasks exercising the scheduler.
 *
 * These tasks run in kernel mode (ring 0) and call scheduler functions
 * directly. They do NOT use the SYSCALL instruction — SYSCALL is a
 * ring-3 → ring-0 transition only; from ring 0 it is undefined.
 */

#include <helios/microprog.h>
#include <helios/scheduler.h>
#include <helios/sched/per_core.h>
#include <helios/pmm.h>
#include <helios/vmm.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/tsc.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Test 1: Yield ping-pong                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void test_yield_task(void) {
    per_core_data_t *core = this_core();
    microprogram_t *mp = core->current;

    for (uint32_t i = 0; i < 10; i++) {
        serial_printf("  TEST: '%s' (id=%u, core=%u) — yield #%u\n",
                      mp->name, mp->id, core->core_id, i);
        scheduler_yield();
    }

    serial_printf("  TEST: '%s' (id=%u) — exiting\n", mp->name, mp->id);
    /* Mark self as dead and let scheduler reclaim */
    scheduler_block(MPROG_STATE_DEAD);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Test 2: Sleep / wake cycle                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void test_sleep_task(void) {
    per_core_data_t *core = this_core();
    microprogram_t *mp = core->current;

    for (uint32_t i = 0; i < 3; i++) {
        serial_printf("  TEST: '%s' (id=%u, core=%u) — sleeping 50ms (#%u)\n",
                      mp->name, mp->id, core->core_id, i);

        /* Set wake time and block with SLEEPING state.
         * scheduler_check_sleeping() will wake us when TSC passes the deadline. */
        mp->wake_time_tsc = rdtsc() + ns_to_tsc(50 * 1000000ULL);  /* 50 ms */
        scheduler_block(MPROG_STATE_SLEEPING);

        serial_printf("  TEST: '%s' (id=%u) — woke up from sleep #%u\n",
                      mp->name, mp->id, i);
    }

    serial_printf("  TEST: '%s' (id=%u) — exiting\n", mp->name, mp->id);
    scheduler_block(MPROG_STATE_DEAD);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Test 3: Priority preemption                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void test_high_priority_task(void) {
    per_core_data_t *core = this_core();
    microprogram_t *mp = core->current;

    serial_printf("  TEST: '%s' (id=%u, prio=%u) — HIGH priority running\n",
                  mp->name, mp->id, (unsigned)mp->priority);

    for (uint32_t i = 0; i < 5; i++) {
        serial_printf("  TEST: '%s' — work unit %u\n", mp->name, i);
        scheduler_yield();
    }

    serial_printf("  TEST: '%s' — exiting\n", mp->name);
    scheduler_block(MPROG_STATE_DEAD);
}

static void test_low_priority_task(void) {
    per_core_data_t *core = this_core();
    microprogram_t *mp = core->current;

    serial_printf("  TEST: '%s' (id=%u, prio=%u) — LOW priority running\n",
                  mp->name, mp->id, (unsigned)mp->priority);

    for (uint32_t i = 0; i < 5; i++) {
        serial_printf("  TEST: '%s' — work unit %u\n", mp->name, i);
        scheduler_yield();
    }

    serial_printf("  TEST: '%s' — exiting\n", mp->name);
    scheduler_block(MPROG_STATE_DEAD);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Stack allocator for test tasks                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define TEST_STACK_PAGES 4  /* 16 KiB per test task */

static uint64_t alloc_test_stack(void) {
    phys_addr_t phys = pmm_alloc_pages(2);  /* order 2 = 4 pages */
    if (phys == 0) return 0;

    virt_addr_t base = 0;
    for (uint32_t i = 0; i < TEST_STACK_PAGES; i++) {
        virt_addr_t va = vmm_heap_alloc_page();
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
        if (vmm_nx_supported()) flags |= PTE_NO_EXECUTE;
        vmm_map_page(va, phys + i * PAGE_SIZE, flags);
        if (i == 0) base = va;
    }
    return base + (uint64_t)TEST_STACK_PAGES * PAGE_SIZE;
}

static microprogram_t *create_test_task(const char *name, priority_t prio,
                                         void (*entry)(void)) {
    uint64_t stack_top = alloc_test_stack();
    if (stack_top == 0) {
        serial_printf("  TEST: stack alloc failed for '%s'\n", name);
        return NULL;
    }

    microprogram_t *mp = microprog_create(name, prio,
                                           (uint64_t)entry, stack_top, 0);
    if (!mp) {
        serial_printf("  TEST: microprog_create failed for '%s'\n", name);
        return NULL;
    }

    mp->stack_base  = stack_top - (uint64_t)TEST_STACK_PAGES * PAGE_SIZE;
    mp->stack_pages = TEST_STACK_PAGES;
    return mp;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Test runner                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

void phase2_test_run(void) {
    serial_puts("\n");
    serial_puts("  ╔══════════════════════════════════════════════╗\n");
    serial_puts("  ║       HELIOS PHASE 2 — TEST SUITE           ║\n");
    serial_puts("  ╚══════════════════════════════════════════════╝\n");
    serial_puts("\n");

    per_core_data_t *core = this_core();

    /* Test 1: Two yield tasks ping-ponging */
    serial_puts("  TEST SUITE: spawning yield-A and yield-B...\n");
    microprogram_t *ya = create_test_task("yield-A", PRIORITY_NORMAL,
                                           test_yield_task);
    microprogram_t *yb = create_test_task("yield-B", PRIORITY_NORMAL,
                                           test_yield_task);
    if (ya) { spinlock_lock(&core->lock); scheduler_enqueue(core, ya); spinlock_unlock(&core->lock); }
    if (yb) { spinlock_lock(&core->lock); scheduler_enqueue(core, yb); spinlock_unlock(&core->lock); }

    /* Test 2: Sleep task */
    serial_puts("  TEST SUITE: spawning sleeper...\n");
    microprogram_t *sl = create_test_task("sleeper", PRIORITY_NORMAL,
                                           test_sleep_task);
    if (sl) { spinlock_lock(&core->lock); scheduler_enqueue(core, sl); spinlock_unlock(&core->lock); }

    /* Test 3: Priority preemption — high should run before low */
    serial_puts("  TEST SUITE: spawning priority-hi and priority-lo...\n");
    microprogram_t *lo = create_test_task("priority-lo", PRIORITY_LOW,
                                           test_low_priority_task);
    microprogram_t *hi = create_test_task("priority-hi", PRIORITY_HIGH,
                                           test_high_priority_task);
    /* Enqueue low first, then high — high should still run first */
    if (lo) { spinlock_lock(&core->lock); scheduler_enqueue(core, lo); spinlock_unlock(&core->lock); }
    if (hi) { spinlock_lock(&core->lock); scheduler_enqueue(core, hi); spinlock_unlock(&core->lock); }

    serial_puts("  TEST SUITE: all test tasks spawned\n\n");
}
