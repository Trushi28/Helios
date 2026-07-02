/**
 * @file syscall.c
 * @brief SYSCALL/SYSRET MSR setup and C dispatcher.
 *
 * Configures per-core MSRs: STAR, LSTAR, SFMASK, and enables EFER.SCE.
 *
 * STAR register value:
 *   STAR[47:32] = 0x08   → SYSCALL: CS=0x08 (kernel code), SS=0x10 (kernel data)
 *   STAR[63:48] = 0x10   → SYSRETQ: CS=0x10+16=0x20|RPL3 (user code),
 *                                    SS=0x10+8 =0x18|RPL3 (user data)
 *   NOT 0x08! Setting STAR[63:48] = 0x08 is a common mistake that
 *   produces CS=0x18 and SS=0x10 — wrong selectors for user mode.
 */

#include <helios/syscall.h>
#include <helios/scheduler.h>
#include <helios/microprog.h>
#include <helios/sched/per_core.h>
#include <helios/pmm.h>
#include <helios/vmm.h>
#include <arch/x86_64/msr.h>
#include <arch/x86_64/tsc.h>
#include <arch/x86_64/paging.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);

/* The SYSCALL entry stub in assembly */
extern void syscall_entry(void);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  MSR addresses                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_CSTAR   0xC0000083  /* compat mode — not used in 64-bit */
#define MSR_SFMASK  0xC0000084

/* RFLAGS bits to clear on SYSCALL entry */
#define RFLAGS_IF   (1ULL << 9)
#define RFLAGS_DF   (1ULL << 10)
#define RFLAGS_TF   (1ULL << 8)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  syscall_init — per-core MSR setup                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */

void syscall_init(void) {
    /* Enable SYSCALL/SYSRET (EFER.SCE = bit 0) */
    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= 1;  /* SCE */
    wrmsr(MSR_IA32_EFER, efer);

    /* LSTAR — 64-bit SYSCALL entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* CSTAR — not used (no 32-bit compat mode) */
    wrmsr(MSR_CSTAR, 0);

    /* STAR — segment selector bases
     * Bits [47:32] = kernel CS base selector = 0x08
     * Bits [63:48] = user CS base selector   = 0x10  (NOT 0x08!)
     *
     * SYSCALL loads: CS = STAR[47:32], SS = STAR[47:32]+8
     *   → CS = 0x08 (kernel code), SS = 0x10 (kernel data) ✓
     *
     * SYSRETQ loads: CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *   → CS = 0x10+16 = 0x20 (user code), SS = 0x10+8 = 0x18 (user data) ✓
     */
    uint64_t star = ((uint64_t)0x0010 << 48) |  /* STAR[63:48] = 0x10 */
                    ((uint64_t)0x0008 << 32);    /* STAR[47:32] = 0x08 */
    wrmsr(MSR_STAR, star);

    /* SFMASK — bits cleared in RFLAGS on SYSCALL entry */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    serial_puts("  SYSCALL: MSRs configured (STAR/LSTAR/SFMASK/EFER.SCE)\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  sys_spawn (S1)                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Spawn a new microprogram from a syscall.
 *
 * For Phase 2, the entry point is a kernel function pointer.
 * Phase 3 will add ELF loading for ring-3 tasks.
 */
static uint64_t sys_spawn(uint64_t entry_rip, uint64_t arg) {
    /* Allocate 4-page (16 KiB) stack */
    phys_addr_t phys = pmm_alloc_pages(2);
    if (phys == 0) return (uint64_t)-1;

    virt_addr_t stack_base = 0;
    for (uint32_t i = 0; i < 4; i++) {
        virt_addr_t va = vmm_heap_alloc_page();
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
        if (vmm_nx_supported()) flags |= PTE_NO_EXECUTE;
        vmm_map_page(va, phys + i * PAGE_SIZE, flags);
        if (i == 0) stack_base = va;
    }
    uint64_t stack_top = stack_base + 4 * PAGE_SIZE;

    microprogram_t *mp = microprog_create("spawned", PRIORITY_NORMAL,
                                           entry_rip, stack_top, arg);
    if (!mp) return (uint64_t)-1;

    mp->stack_base  = stack_base;
    mp->stack_pages = 4;

    per_core_data_t *core = this_core();
    spinlock_lock(&core->lock);
    scheduler_enqueue(core, mp);
    spinlock_unlock(&core->lock);

    return (uint64_t)mp->id;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  C dispatcher                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

uint64_t syscall_dispatch(uint64_t syscall_nr, uint64_t arg1,
                           uint64_t arg2, uint64_t arg3) {
    (void)arg3;  /* unused in Phase 2 */

    switch (syscall_nr) {
    case SYS_YIELD:
        scheduler_yield();
        return 0;

    case SYS_EXIT: {
        per_core_data_t *core = this_core();
        microprogram_t *mp = core->current;
        if (mp && mp != core->idle_thread) {
            microprog_destroy(mp);
            core->current = NULL;
            scheduler_preempt();
        }
        /* Should not reach here */
        return 0;
    }

    case SYS_SLEEP: {
        /* arg1 = sleep duration in nanoseconds */
        per_core_data_t *core = this_core();
        microprogram_t *mp = core->current;
        if (mp) {
            mp->wake_time_tsc = rdtsc() + ns_to_tsc(arg1);
            scheduler_block(MPROG_STATE_SLEEPING);
        }
        return 0;
    }

    case SYS_SPAWN:
        /* arg1 = entry_rip, arg2 = arg */
        return sys_spawn(arg1, arg2);

    default:
        serial_printf("  SYSCALL: unknown syscall %lu\n", syscall_nr);
        return (uint64_t)-1;
    }
}
