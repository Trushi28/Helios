/**
 * @file gdt.c
 * @brief 64-bit GDT setup for Helios — BSP and AP TSS management.
 *
 * GDT layout (SYSRETQ-compatible ordering):
 *   [0]   Null descriptor
 *   [1]   Kernel code (64-bit, Ring 0)      selector = 0x08
 *   [2]   Kernel data (Ring 0)              selector = 0x10
 *   [3]   User data   (Ring 3)              selector = 0x18
 *   [4]   User code   (64-bit, Ring 3)      selector = 0x20
 *   [5-6] BSP TSS descriptor (16 bytes)     selector = 0x28
 *   [7+]  Per-AP TSS descriptors (2 entries each)
 *
 * SYSRETQ segment computation (Intel SDM Vol. 2B, SYSRET):
 *   STAR[63:48] = 0x10  (base selector for user mode)
 *   CS = STAR[63:48] + 16 = 0x20 | RPL3 = user code  ✓
 *   SS = STAR[63:48] +  8 = 0x18 | RPL3 = user data  ✓
 */

#include <arch/x86_64/gdt.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern void *memset(void *dest, int val, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  GDT Entry                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   /* flags + limit_high nibble */
    uint8_t  base_high;
} PACKED gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED gdt_ptr_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  GDT Table — sized for BSP + MAX_CPUS AP TSS descriptors                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Layout: 5 standard entries + 2 for BSP TSS + 2 per AP.
 * Each TSS descriptor is 16 bytes = 2 gdt_entry_t.
 */
#define GDT_FIXED_ENTRIES  5
#define GDT_TSS_ENTRIES    2  /* per TSS descriptor */
#define GDT_TOTAL_ENTRIES  (GDT_FIXED_ENTRIES + (1 + HELIOS_MAX_CPUS) * GDT_TSS_ENTRIES)

static gdt_entry_t g_gdt[GDT_TOTAL_ENTRIES];
static gdt_ptr_t   g_gdt_ptr;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  BSP TSS and IST stacks                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* IST stack sizes */
#define IST_DF_SIZE   16384   /* IST1: double fault — 16 KiB */
#define IST_NMI_SIZE   8192   /* IST2: NMI — 8 KiB */
#define IST_MC_SIZE    8192   /* IST3: machine check — 8 KiB */
#define IST_DB_SIZE    8192   /* IST4: debug — 8 KiB */
#define IST_PF_SIZE   16384   /* IST5: page fault — 16 KiB (must always be
                                * able to run interrupt_dispatch()'s guard-
                                * page check plus panic_with_frame()'s
                                * serial_printf() calls even when the
                                * interrupted kernel stack has overflowed;
                                * sized the same as IST1 since it serves the
                                * same "last line of defense" role) */

static ALIGNED(16) uint8_t ist_df_stack[IST_DF_SIZE];
static ALIGNED(16) uint8_t ist_nmi_stack[IST_NMI_SIZE];
static ALIGNED(16) uint8_t ist_mc_stack[IST_MC_SIZE];
static ALIGNED(16) uint8_t ist_db_stack[IST_DB_SIZE];
static ALIGNED(16) uint8_t ist_pf_stack[IST_PF_SIZE];

static tss_t g_bsp_tss;

/* Per-AP TSS storage */
static tss_t g_ap_tss[HELIOS_MAX_CPUS];

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  TSS descriptor writer                                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Write a 16-byte TSS descriptor into two adjacent GDT entries.
 *
 * x86-64 TSS descriptors are 16 bytes (two GDT entries) and contain
 * the base address split across 4 fields plus the limit.
 */
static void write_tss_descriptor(uint32_t gdt_index, const tss_t *tss) {
    uint64_t base  = (uint64_t)tss;
    uint32_t limit = sizeof(tss_t) - 1;

    /* Low 8 bytes (normal GDT entry format) */
    gdt_entry_t *low = &g_gdt[gdt_index];
    low->limit_low   = (uint16_t)(limit & 0xFFFF);
    low->base_low    = (uint16_t)(base & 0xFFFF);
    low->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    low->access      = 0x89;       /* Present, 64-bit TSS available */
    low->granularity  = (uint8_t)((limit >> 16) & 0x0F);
    low->base_high   = (uint8_t)((base >> 24) & 0xFF);

    /* High 8 bytes — base[63:32] + reserved */
    uint32_t *high = (uint32_t *)&g_gdt[gdt_index + 1];
    high[0] = (uint32_t)(base >> 32);
    high[1] = 0;  /* reserved */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  GDT Installation (BSP)                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

void gdt_install(void) {
    /* Zero the entire GDT */
    memset(g_gdt, 0, sizeof(g_gdt));

    /* [0] Null descriptor — already zero */

    /* [1] Kernel code (Ring 0, 64-bit): access=0x9A, flags=L=1 */
    g_gdt[1] = (gdt_entry_t){ 0xFFFF, 0, 0, 0x9A, 0x20, 0 };

    /* [2] Kernel data (Ring 0): access=0x92 */
    g_gdt[2] = (gdt_entry_t){ 0xFFFF, 0, 0, 0x92, 0x00, 0 };

    /* [3] User data (Ring 3): access=0xF2
     * MUST precede user code for SYSRETQ: SS = STAR[63:48]+8 = 0x18 */
    g_gdt[3] = (gdt_entry_t){ 0xFFFF, 0, 0, 0xF2, 0x00, 0 };

    /* [4] User code (Ring 3, 64-bit): access=0xFA, flags=L=1
     * SYSRETQ: CS = STAR[63:48]+16 = 0x20 */
    g_gdt[4] = (gdt_entry_t){ 0xFFFF, 0, 0, 0xFA, 0x20, 0 };

    /* ── Initialize BSP TSS ─────────────────────────────────────────────── */
    memset(&g_bsp_tss, 0, sizeof(g_bsp_tss));

    /* IST1: Double fault (#DF, vector 8) */
    g_bsp_tss.ist[0] = (uint64_t)&ist_df_stack[IST_DF_SIZE];
    /* IST2: NMI (vector 2) */
    g_bsp_tss.ist[1] = (uint64_t)&ist_nmi_stack[IST_NMI_SIZE];
    /* IST3: Machine check (#MC, vector 18) */
    g_bsp_tss.ist[2] = (uint64_t)&ist_mc_stack[IST_MC_SIZE];
    /* IST4: Debug (#DB, vector 1) */
    g_bsp_tss.ist[3] = (uint64_t)&ist_db_stack[IST_DB_SIZE];
    /* IST5: Page fault (#PF, vector 14) — see idt.c idt_install() and
     * docs/14-INTERRUPTS.md. Required so the guard page below the kernel
     * stack can always be reported cleanly instead of cascading. */
    g_bsp_tss.ist[4] = (uint64_t)&ist_pf_stack[IST_PF_SIZE];

    /* RSP0: kernel stack for ring 3 → ring 0 transition.
     * Will be updated by scheduler on context switch. */
    extern char kernel_stack_top[];
    g_bsp_tss.rsp0 = (uint64_t)kernel_stack_top;

    /* IOPB offset set past TSS limit = deny all I/O ports */
    g_bsp_tss.iopb_offset = sizeof(tss_t);

    /* [5-6] BSP TSS descriptor */
    write_tss_descriptor(GDT_FIXED_ENTRIES, &g_bsp_tss);

    /* ── Load GDTR ──────────────────────────────────────────────────────── */
    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base  = (uint64_t)&g_gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"

        /* Reload CS via a far return */
        "pushq $0x08\n\t"           /* GDT_SEL_KERNEL_CODE */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"

        /* Reload data segment registers */
        "movw $0x10, %%ax\n\t"     /* GDT_SEL_KERNEL_DATA */
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"

        /* FS and GS to null for now (GS set per-core in Phase 2) */
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "r"(&g_gdt_ptr)
        : "rax", "memory"
    );

    /* ── Load Task Register ─────────────────────────────────────────────── */
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)GDT_SEL_TSS) : "memory");

    serial_puts("  GDT: installed with BSP TSS (IST1-5 active)\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  RSP0 update                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

void gdt_set_tss_rsp0(uint64_t rsp0) {
    g_bsp_tss.rsp0 = rsp0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Per-AP TSS installation (called from ap_entry)                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

void gdt_install_ap_tss(uint32_t ap_index, uint64_t ist1_top, uint64_t ist5_top,
                        uint64_t rsp0) {
    if (ap_index >= HELIOS_MAX_CPUS) return;

    tss_t *tss = &g_ap_tss[ap_index];
    memset(tss, 0, sizeof(tss_t));

    tss->ist[0] = ist1_top;    /* IST1: double fault */
    /* IST5: page fault. idt_install()'s IDT is shared by every core, and
     * its vector-14 gate always requests IST5; if this AP's slot were left
     * zero, the AP's very first #PF (e.g. an ordinary demand-paged heap
     * fault from vmm_heap_alloc_page()) would load RSP=0 and triple-fault
     * (see docs/14-INTERRUPTS.md and the "IST entries must be backed
     * before exceptions fire" note in vmm.c). */
    tss->ist[4] = ist5_top;
    tss->rsp0   = rsp0;
    tss->iopb_offset = sizeof(tss_t);

    /* GDT index = 5 (BSP TSS) + 2 + ap_index * 2 */
    uint32_t gdt_idx = GDT_FIXED_ENTRIES + GDT_TSS_ENTRIES + ap_index * GDT_TSS_ENTRIES;
    write_tss_descriptor(gdt_idx, tss);

    /* Load TR with the AP's TSS selector */
    uint16_t tss_sel = (uint16_t)(gdt_idx * sizeof(gdt_entry_t));
    __asm__ volatile("ltr %w0" : : "r"(tss_sel) : "memory");

    serial_printf("  GDT: AP %u TSS at GDT[%u], selector 0x%x\n",
                  ap_index, gdt_idx, (unsigned)tss_sel);
}
