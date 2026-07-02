/**
 * @file smp.c
 * @brief SMP initialization — AP bringup via INIT-SIPI-SIPI.
 *
 * Copies the AP trampoline binary to physical 0x8000 (already reserved
 * below 1 MiB — the PMM never adds this region). Populates the data
 * block at AP_DATA_OFFSET with CR3, stack, and entry pointers. Sends
 * INIT-SIPI-SIPI to each AP and waits for the ap_ready flag.
 */

#include <helios/smp.h>
#include <helios/acpi/madt.h>
#include <helios/sched/per_core.h>
#include <helios/scheduler.h>
#include <helios/pmm.h>
#include <helios/vmm.h>
#include <arch/x86_64/x2apic.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/tsc.h>
#include <arch/x86_64/paging.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern void *memset(void *dest, int val, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern NORETURN void panic(const char *msg);

/* Syscall init — per-core MSRs (LSTAR, STAR, SFMASK, EFER.SCE) */
extern void syscall_init(void);

/* AP trampoline binary blob — linked via objcopy */
extern uint8_t _binary_ap_trampoline_bin_start[];
extern uint8_t _binary_ap_trampoline_bin_end[];

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Trampoline data block layout (matches ap_trampoline.asm)                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct PACKED {
    uint64_t cr3;           /* +0x00 */
    uint64_t stack_top;     /* +0x08 */
    uint64_t entry_addr;    /* +0x10 */
    /* GDT pointer: 2-byte limit + 8-byte base = 10 bytes, padded to 16 */
    uint16_t gdt_limit;     /* +0x18 */
    uint64_t gdt_base;      /* +0x1A */
    uint8_t  _pad[6];       /* +0x22 → align to 0x28 */
    volatile uint32_t ap_ready;   /* +0x28 */
    uint32_t ap_index;      /* +0x2C */
} ap_data_block_t;

_Static_assert(sizeof(ap_data_block_t) == 0x30,
               "AP data block size mismatch with trampoline ASM");

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  AP kernel stack allocation                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define AP_STACK_PAGES 4   /* 16 KiB per AP */

static uint64_t alloc_ap_stack(void) {
    phys_addr_t phys = pmm_alloc_pages(2);  /* order 2 = 4 pages */
    if (phys == 0) return 0;

    virt_addr_t base = 0;
    for (uint32_t i = 0; i < AP_STACK_PAGES; i++) {
        virt_addr_t va = vmm_heap_alloc_page();
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
        if (vmm_nx_supported()) flags |= PTE_NO_EXECUTE;
        vmm_map_page(va, phys + i * PAGE_SIZE, flags);
        if (i == 0) base = va;
    }

    return base + (uint64_t)AP_STACK_PAGES * PAGE_SIZE;  /* stack top */
}

/* Small dedicated IST stack allocator, reused for each per-AP IST slot
 * (IST1 double-fault, IST5 page-fault — see ap_entry() below). Each call
 * allocates a fresh, separate stack; IST slots must never share memory. */
#define AP_IST_PAGES 2  /* 8 KiB */

static uint64_t alloc_ap_ist_stack(void) {
    phys_addr_t phys = pmm_alloc_pages(1);  /* order 1 = 2 pages */
    if (phys == 0) return 0;

    virt_addr_t base = 0;
    for (uint32_t i = 0; i < AP_IST_PAGES; i++) {
        virt_addr_t va = vmm_heap_alloc_page();
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
        if (vmm_nx_supported()) flags |= PTE_NO_EXECUTE;
        vmm_map_page(va, phys + i * PAGE_SIZE, flags);
        if (i == 0) base = va;
    }

    return base + (uint64_t)AP_IST_PAGES * PAGE_SIZE;  /* stack top */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  AP C entry point                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void ap_entry(uint32_t ap_index) {
    /* Initialize x2APIC on this core */
    x2apic_init();

    uint32_t apic_id = x2apic_get_id();
    serial_printf("  SMP: AP %u online (x2APIC ID %u)\n", ap_index, apic_id);

    /* Install per-AP TSS with IST1 (double-fault) and IST5 (page-fault).
     * Both must be backed before this AP re-enables interrupts: the IDT
     * is shared across all cores, and its #PF gate always requests IST5
     * (idt.c), so an AP with ist[4] left at 0 would triple-fault on its
     * first page fault — including the routine demand-paging fault that
     * vmm_heap_alloc_page() triggers a few lines below. */
    uint64_t ist1_top = alloc_ap_ist_stack();
    uint64_t ist5_top = alloc_ap_ist_stack();
    uint64_t rsp0 = 0;  /* updated by scheduler_init_core */
    gdt_install_ap_tss(ap_index, ist1_top, ist5_top, rsp0);

    /* Initialize per-core data */
    per_core_data_t *core = per_core_init(ap_index + 1, apic_id);
    if (!core) {
        serial_printf("  SMP: AP %u — per_core_init failed, halting\n", ap_index);
        cli();
        for (;;) cpu_halt();
    }

    /* Set GS base for this_core() */
    per_core_set_gsbase(core);

    /* Set up per-core SYSCALL MSRs (LSTAR, STAR, SFMASK, EFER.SCE) */
    syscall_init();

    /* Initialize per-AP scheduler (creates idle thread, arms timer, sti) */
    scheduler_init_core();

    /* Signal BSP that this AP is ready */
    volatile uint32_t *ap_ready =
        (volatile uint32_t *)(KERNEL_PHYS_MAP_BASE +
                              AP_TRAMPOLINE_PHYS + AP_DATA_OFFSET + 0x28);
    *ap_ready = 1;

    /* Mark this CPU as online in the global table */
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpu_table[i].x2apic_id == apic_id) {
            g_cpu_table[i].online = true;
            break;
        }
    }

    /* Enter the idle loop — never returns */
    scheduler_idle_loop();
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Delay helper                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void delay_us(uint64_t us) {
    uint64_t target = rdtsc() + ns_to_tsc(us * 1000);
    while (rdtsc() < target) {
        cpu_pause();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  SMP init — called from kernel_main on BSP                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

void smp_init(void) {
    uint32_t bsp_apic_id = x2apic_get_id();

    /* Count APs */
    uint32_t ap_count = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpu_table[i].x2apic_id != bsp_apic_id && g_cpu_table[i].enabled) {
            ap_count++;
        }
    }

    if (ap_count == 0) {
        serial_puts("  SMP: no APs found — uniprocessor system\n");
        return;
    }

    serial_printf("  SMP: %u APs to boot (BSP x2APIC ID %u)\n",
                  ap_count, bsp_apic_id);

    /* ── Copy trampoline to physical 0x8000 ──────────────────────────── */
    /* The direct map gives us virtual access: KERNEL_PHYS_MAP_BASE + 0x8000 */
    uint8_t *tramp_dest = (uint8_t *)(KERNEL_PHYS_MAP_BASE + AP_TRAMPOLINE_PHYS);
    size_t tramp_size = (size_t)(_binary_ap_trampoline_bin_end -
                                  _binary_ap_trampoline_bin_start);

    memset(tramp_dest, 0, PAGE_SIZE);
    memcpy(tramp_dest, _binary_ap_trampoline_bin_start, tramp_size);

    serial_printf("  SMP: trampoline copied to 0x%lx (%zu bytes)\n",
                  (uint64_t)AP_TRAMPOLINE_PHYS, tramp_size);

    /* ── Get current CR3 and GDT for the data block ──────────────────── */
    uint64_t cr3_val = read_cr3();

    /* Read the current GDTR */
    struct PACKED { uint16_t limit; uint64_t base; } gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));

    /* ── Boot each AP ────────────────────────────────────────────────── */
    uint32_t ap_idx = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpu_table[i].x2apic_id == bsp_apic_id || !g_cpu_table[i].enabled)
            continue;

        uint32_t target_apic = g_cpu_table[i].x2apic_id;

        /* Allocate per-AP kernel stack */
        uint64_t stack_top = alloc_ap_stack();
        if (stack_top == 0) {
            serial_printf("  SMP: AP %u — stack alloc failed, skipping\n", ap_idx);
            ap_idx++;
            continue;
        }

        /* Populate the data block */
        ap_data_block_t *data =
            (ap_data_block_t *)(tramp_dest + AP_DATA_OFFSET);
        data->cr3        = cr3_val;
        data->stack_top  = stack_top;
        data->entry_addr = (uint64_t)ap_entry;
        data->gdt_limit  = gdtr.limit;
        data->gdt_base   = gdtr.base;
        data->ap_ready   = 0;
        data->ap_index   = ap_idx;

        /* ── INIT-SIPI-SIPI sequence ─────────────────────────────────── */
        /* Send INIT IPI */
        x2apic_send_init(target_apic);
        delay_us(10000);  /* 10 ms delay after INIT */

        /* Send first SIPI */
        x2apic_send_sipi(target_apic, AP_SIPI_VECTOR);
        delay_us(200);    /* 200 μs between SIPIs */

        /* Send second SIPI (Intel spec requires two) */
        x2apic_send_sipi(target_apic, AP_SIPI_VECTOR);

        /* Wait for AP to signal ready (timeout ~1 second) */
        uint64_t timeout = rdtsc() + ns_to_tsc(1000000000ULL);
        while (data->ap_ready == 0 && rdtsc() < timeout) {
            cpu_pause();
        }

        if (data->ap_ready) {
            serial_printf("  SMP: AP %u (x2APIC %u) — ready\n",
                          ap_idx, target_apic);
        } else {
            serial_printf("  SMP: AP %u (x2APIC %u) — TIMEOUT, not responding\n",
                          ap_idx, target_apic);
        }

        ap_idx++;
    }

    serial_printf("  SMP: boot complete — %u APs initialized\n", ap_idx);
}
