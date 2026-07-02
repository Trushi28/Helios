/**
 * @file microprog.c
 * @brief Microprogram lifecycle — create and destroy schedulable tasks.
 *
 * Allocates microprogram_t from the "microprogram" slab cache (1024 bytes).
 * Pre-populates the kernel stack so that context_switch()'s pop sequence
 * restores callee-saved regs and the final `ret` jumps to the entry point.
 */

#include <helios/microprog.h>
#include <helios/slab.h>
#include <helios/pmm.h>
#include <helios/vmm.h>
#include <helios/types.h>
#include <arch/x86_64/paging.h>

extern void serial_printf(const char *fmt, ...);
extern void *memset(void *dest, int val, size_t n);
extern char *strncpy(char *dest, const char *src, size_t n);
extern NORETURN void panic(const char *msg);

static uint32_t g_next_mprog_id = 1;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Timeslice lookup                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t priority_to_timeslice(priority_t p) {
    switch (p) {
    case PRIORITY_REALTIME: return TIMESLICE_REALTIME_NS;
    case PRIORITY_HIGH:     return TIMESLICE_HIGH_NS;
    case PRIORITY_NORMAL:   return TIMESLICE_NORMAL_NS;
    case PRIORITY_LOW:      return TIMESLICE_LOW_NS;
    case PRIORITY_IDLE:     return TIMESLICE_IDLE_NS;
    default:                return TIMESLICE_NORMAL_NS;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Task entry wrapper                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Wrapper that calls the actual entry function and handles return.
 *
 * context_switch's ret lands here with arg in RDI. We call the real entry
 * function (stored in R12 by the stack pre-population) and if it returns,
 * we destroy the task.
 */
static NORETURN void microprog_entry_trampoline(void) {
    /* R12 = entry_rip, RDI = arg (set by stack pre-pop) */
    /* The entry function is called directly by the ret in context_switch.
     * If it returns, we need to clean up. This function exists as a
     * safety net registered as the return address. */
    /* A real task shouldn't return here — the scheduler handles cleanup
     * via SYS_EXIT / microprog_destroy. If we do arrive here, panic. */
    panic("microprog: task returned without calling sys_exit");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Create                                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

microprogram_t *microprog_create(const char *name, priority_t priority,
                                  uint64_t entry_rip, uint64_t stack_top,
                                  uint64_t arg) {
    slab_cache_t *cache = slab_get_cache("microprogram");
    if (!cache) {
        panic("microprog_create: 'microprogram' slab cache not found");
    }

    microprogram_t *mp = (microprogram_t *)slab_alloc(cache);
    if (!mp) return NULL;

    memset(mp, 0, sizeof(microprogram_t));
    mp->id       = g_next_mprog_id++;
    mp->state    = MPROG_STATE_READY;
    mp->priority = priority;
    mp->affinity_core = 0xFFFFFFFF;  /* no affinity */
    mp->timeslice_ns  = priority_to_timeslice(priority);
    strncpy(mp->name, name, MPROG_NAME_MAX - 1);
    mp->name[MPROG_NAME_MAX - 1] = '\0';

    /* ── Pre-populate stack for context_switch ────────────────────────── */
    /*
     * context_switch() does:
     *   push r15, r14, r13, r12, rbx, rbp
     *   save rsp to save_to->rsp
     *   load rsp from restore_from->rsp
     *   pop rbp, rbx, r12, r13, r14, r15
     *   ret
     *
     * So we need the stack (growing downward) to contain:
     *   [stack_top - 8]  = entry_rip        (ret target)
     *   [stack_top - 16] = 0                (r15)
     *   [stack_top - 24] = 0                (r14)
     *   [stack_top - 32] = arg              (r13 — not directly used, but preserved)
     *   [stack_top - 40] = 0                (r12)
     *   [stack_top - 48] = 0                (rbx)
     *   [stack_top - 56] = 0                (rbp)
     *
     * After context_switch pops, RIP = entry_rip, and the task starts.
     * RDI (first argument) is set by the scheduler before switching.
     */
    uint64_t *sp = (uint64_t *)stack_top;

    /* Return address — where `ret` in context_switch jumps to */
    *(--sp) = entry_rip;

    /* Callee-saved regs in pop order (rbp, rbx, r12, r13, r14, r15)
     * but pushed in reverse order, so on stack bottom-up:
     * r15, r14, r13, r12, rbx, rbp */
    *(--sp) = 0;             /* r15 */
    *(--sp) = 0;             /* r14 */
    *(--sp) = 0;             /* r13 */
    *(--sp) = 0;             /* r12 */
    *(--sp) = 0;             /* rbx */
    *(--sp) = 0;             /* rbp */

    mp->ctx.rsp = (uint64_t)sp;

    (void)arg;  /* arg is passed via scheduler when setting RDI */
    (void)microprog_entry_trampoline; /* suppress unused warning */

    serial_printf("  MPROG: created '%s' (id=%u, prio=%u)\n",
                  mp->name, mp->id, (unsigned)mp->priority);
    return mp;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Destroy                                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

void microprog_destroy(microprogram_t *mp) {
    if (!mp) return;

    mp->state = MPROG_STATE_DEAD;

    /* Free the stack pages if we own them */
    if (mp->stack_base && mp->stack_pages > 0) {
        /* Determine order from page count */
        uint32_t order = 0;
        uint32_t pages = mp->stack_pages;
        while (pages > 1) { pages >>= 1; order++; }

        phys_addr_t phys = vmm_virt_to_phys(mp->stack_base);
        if (phys != 0) {
            /* Unmap all stack pages */
            for (uint32_t i = 0; i < mp->stack_pages; i++) {
                vmm_unmap_page(mp->stack_base + i * PAGE_SIZE);
            }
            pmm_free_pages(phys, order);
        }
    }

    /* Free the microprogram struct back to slab */
    slab_cache_t *cache = slab_get_cache("microprogram");
    if (cache) {
        slab_free(cache, mp);
    }
}
