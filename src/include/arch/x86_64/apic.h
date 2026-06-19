/**
 * @file apic.h
 * @brief x2APIC register definitions and MSR addresses.
 */

#ifndef ARCH_X86_64_APIC_H
#define ARCH_X86_64_APIC_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  APIC Base MSR bits                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define APIC_BASE_BSP               (1ULL << 8)
#define APIC_BASE_GLOBAL_ENABLE     (1ULL << 11)
#define APIC_BASE_X2APIC_ENABLE     (1ULL << 10)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  x2APIC MSR addresses                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define MSR_X2APIC_ID               0x802
#define MSR_X2APIC_VERSION          0x803
#define MSR_X2APIC_TPR              0x808
#define MSR_X2APIC_PPR              0x80A
#define MSR_X2APIC_EOI              0x80B
#define MSR_X2APIC_LDR              0x80D
#define MSR_X2APIC_SIVR             0x80F
#define MSR_X2APIC_ISR0             0x810
#define MSR_X2APIC_TMR0             0x818
#define MSR_X2APIC_IRR0             0x820
#define MSR_X2APIC_ESR              0x828
#define MSR_X2APIC_ICR              0x830
#define MSR_X2APIC_LVT_TIMER        0x832
#define MSR_X2APIC_LVT_THERMAL      0x833
#define MSR_X2APIC_LVT_PMC          0x834
#define MSR_X2APIC_LVT_LINT0        0x835
#define MSR_X2APIC_LVT_LINT1        0x836
#define MSR_X2APIC_LVT_ERROR        0x837
#define MSR_X2APIC_TIMER_INIT        0x838
#define MSR_X2APIC_TIMER_CURRENT     0x839
#define MSR_X2APIC_TIMER_DIV         0x83E
#define MSR_X2APIC_SELF_IPI          0x83F

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  SIVR bits                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define APIC_SIVR_ENABLE            (1U << 8)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  LVT delivery modes                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define APIC_DELIVERY_FIXED         0x00
#define APIC_DELIVERY_SMI           0x02
#define APIC_DELIVERY_NMI           0x04
#define APIC_DELIVERY_INIT          0x05
#define APIC_DELIVERY_STARTUP       0x06
#define APIC_DELIVERY_EXTINT        0x07

#define APIC_LVT_MASKED             (1U << 16)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  IPI vectors (Helios-defined)                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define IPI_RESCHEDULE              0xF0
#define IPI_TLB_SHOOTDOWN           0xF1
#define IPI_HALT                    0xF2
#define IPI_PANIC                   0xF3
#define SPURIOUS_VECTOR             0xFE

#endif /* ARCH_X86_64_APIC_H */
