/**
 * @file per_core.c
 * @brief Per-core data allocation and GS base setup.
 *
 * Allocates a page for each core's per_core_data_t, maps it with NX,
 * and sets the GS_BASE / KERNEL_GS_BASE MSRs for swapgs support.
 */

#include <helios/sched/per_core.h>
#include <helios/microprog.h>
#include <arch/x86_64/msr.h>
#include <arch/x86_64/paging.h>
#include <helios/pmm.h>
#include <helios/vmm.h>
#include <helios/types.h>

/* Verify the hardcoded offset in microprog.h matches the actual struct */
_Static_assert(__builtin_offsetof(per_core_data_t, current) == PER_CORE_CURRENT_OFFSET,
               "PER_CORE_CURRENT_OFFSET in microprog.h is out of sync with per_core_data_t");

extern void serial_printf(const char *fmt, ...);
extern void *memset(void *dest, int val, size_t n);

per_core_data_t *per_core_init(uint32_t core_id, uint32_t x2apic_id) {
    /* Allocate a physical page for the per-core data */
    phys_addr_t phys = pmm_alloc_pages(0);
    if (phys == 0) return NULL;

    /* Map into kernel heap with NX */
    virt_addr_t va = vmm_heap_alloc_page();
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
    if (vmm_nx_supported()) flags |= PTE_NO_EXECUTE;
    vmm_map_page(va, phys, flags);

    /* Initialize the structure */
    per_core_data_t *data = (per_core_data_t *)va;
    memset(data, 0, PAGE_SIZE);

    data->self            = data;
    data->kernel_stack_top = 0;    /* set by scheduler_init/scheduler_init_core */
    data->user_rsp_save    = 0;
    data->core_id          = core_id;
    data->x2apic_id        = x2apic_id;
    data->current           = NULL;
    data->priority_bitmap   = 0;
    data->idle_thread       = NULL;
    data->sleeping_head     = NULL;
    data->sleeping_count    = 0;
    data->context_switches  = 0;
    data->steals            = 0;
    data->lock              = (spinlock_t)SPINLOCK_INIT;

    serial_printf("  PER_CORE: core %u (x2APIC %u) at %p\n",
                  core_id, x2apic_id, (void *)data);
    return data;
}

void per_core_set_gsbase(per_core_data_t *data) {
    /* GS_BASE = current GS base (used in kernel mode).
     * KERNEL_GS_BASE = swapped to on swapgs (from user → kernel).
     * We set both so swapgs in the SYSCALL entry path works. */
    wrmsr(MSR_IA32_GS_BASE, (uint64_t)data);
    wrmsr(MSR_IA32_KERNEL_GS_BASE, (uint64_t)data);
}
