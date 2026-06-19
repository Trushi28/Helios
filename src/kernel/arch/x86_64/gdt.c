/**
 * @file gdt.c
 * @brief 64-bit GDT setup for Helios.
 *
 * GDT entries:
 *   [0] Null descriptor
 *   [1] Kernel code (64-bit, Ring 0)
 *   [2] Kernel data (Ring 0)
 *   [3] User code   (64-bit, Ring 3) — placeholder for Phase 2
 *   [4] User data   (Ring 3)         — placeholder for Phase 2
 *   [5-6] TSS descriptor (16 bytes)  — placeholder for Phase 2
 */

#include <helios/types.h>

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
/*  GDT Table                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Access byte:
 *   P=1 DPL=xx S=1 Type=xxxx
 *   Code: 0x9A (Ring 0), 0xFA (Ring 3)
 *   Data: 0x92 (Ring 0), 0xF2 (Ring 3)
 *
 * Granularity/flags:
 *   For 64-bit code: G=0 D=0 L=1 (0x20 in upper nibble)
 *   For data:        G=0 D=0 L=0 (0x00)
 */

static gdt_entry_t g_gdt[] = {
    /* [0] Null descriptor */
    { 0, 0, 0, 0x00, 0x00, 0 },

    /* [1] Kernel code (Ring 0, 64-bit): access=0x9A, flags=L=1 */
    { 0xFFFF, 0, 0, 0x9A, 0x20, 0 },

    /* [2] Kernel data (Ring 0): access=0x92 */
    { 0xFFFF, 0, 0, 0x92, 0x00, 0 },

    /* [3] User code (Ring 3, 64-bit): access=0xFA, flags=L=1 */
    { 0xFFFF, 0, 0, 0xFA, 0x20, 0 },

    /* [4] User data (Ring 3): access=0xF2 */
    { 0xFFFF, 0, 0, 0xF2, 0x00, 0 },

    /* [5-6] TSS — filled in Phase 2 (SMP per-core setup) */
    { 0, 0, 0, 0x00, 0x00, 0 },
    { 0, 0, 0, 0x00, 0x00, 0 },
};

static gdt_ptr_t g_gdt_ptr;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  GDT Installation                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Install the GDT and reload segment registers.
 *
 * Called from entry.asm during early boot.
 * After LGDT, we do a far return to reload CS and set DS/ES/SS/FS/GS.
 */
void gdt_install(void) {
    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base  = (uint64_t)&g_gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"

        /* Reload CS via a far return */
        "pushq $0x08\n\t"           /* Kernel code segment selector */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"

        /* Reload data segment registers */
        "movw $0x10, %%ax\n\t"     /* Kernel data segment selector */
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"

        /* FS and GS to null for now (set per-core in Phase 2) */
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "r"(&g_gdt_ptr)
        : "rax", "memory"
    );
}
