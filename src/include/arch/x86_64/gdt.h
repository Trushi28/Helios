/**
 * @file gdt.h
 * @brief x86-64 GDT segment selectors, TSS structure, and GDT management API.
 *
 * Segment ordering: null, kernel code, kernel data, user data, user code, TSS.
 * User data precedes user code so SYSRETQ computes correct selectors:
 *   CS = STAR[63:48] + 16 = 0x10 + 16 = 0x20 | RPL3 = user code
 *   SS = STAR[63:48] +  8 = 0x10 +  8 = 0x18 | RPL3 = user data
 */

#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  GDT Selectors                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define GDT_SEL_NULL        0x00
#define GDT_SEL_KERNEL_CODE 0x08
#define GDT_SEL_KERNEL_DATA 0x10
#define GDT_SEL_USER_DATA   0x18
#define GDT_SEL_USER_CODE   0x20
#define GDT_SEL_TSS         0x28   /* BSP TSS; AP TSS at higher indices */

/* Maximum CPU count for sizing the GDT */
#define HELIOS_MAX_CPUS     256

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  TSS (Task State Segment)                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct PACKED {
    uint32_t reserved0;
    uint64_t rsp0;         /**< Stack pointer for ring 0 transition */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];       /**< IST1–IST7 stack pointers */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;  /**< Offset to I/O permission bitmap */
} tss_t;

_Static_assert(sizeof(tss_t) == 104, "TSS size must be 104 bytes");

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  API                                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Install the GDT with BSP TSS and reload segment registers.
 * Called once during BSP boot from entry.asm.
 */
void gdt_install(void);

/**
 * @brief Set the RSP0 field in the BSP TSS.
 * Used to update the kernel stack pointer for ring transitions.
 */
void gdt_set_tss_rsp0(uint64_t rsp0);

/**
 * @brief Install a per-AP TSS descriptor in the GDT and load TR.
 *
 * Must be called from each AP during SMP bringup (ap_entry).
 * @param ap_index  Logical AP index (0 = first AP, not BSP).
 * @param ist1_top  Top of the AP's IST1 stack (double-fault).
 * @param ist5_top  Top of the AP's IST5 stack (page fault — see idt.c;
 *                  every core's #PF vector routes through IST5, so this
 *                  MUST be a valid, mapped stack before this AP re-enables
 *                  interrupts, or the AP's first #PF loads RSP=0 from an
 *                  unset TSS slot and triple-faults).
 * @param rsp0      Initial kernel stack pointer for the AP.
 */
void gdt_install_ap_tss(uint32_t ap_index, uint64_t ist1_top, uint64_t ist5_top,
                        uint64_t rsp0);

#endif /* ARCH_X86_64_GDT_H */
