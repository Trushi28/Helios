/**
 * @file msr.h
 * @brief x86-64 Model-Specific Register access and definitions.
 */

#ifndef ARCH_X86_64_MSR_H
#define ARCH_X86_64_MSR_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  MSR access                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

static ALWAYS_INLINE uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static ALWAYS_INLINE void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Common MSR addresses                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define MSR_IA32_APIC_BASE          0x0000001B
#define MSR_IA32_EFER               0xC0000080
#define MSR_IA32_STAR               0xC0000081
#define MSR_IA32_LSTAR              0xC0000082
#define MSR_IA32_CSTAR              0xC0000083
#define MSR_IA32_SFMASK             0xC0000084
#define MSR_IA32_FS_BASE            0xC0000100
#define MSR_IA32_GS_BASE            0xC0000101
#define MSR_IA32_KERNEL_GS_BASE     0xC0000102
#define MSR_IA32_TSC_AUX            0xC0000103
#define MSR_IA32_THERM_STATUS       0x0000019C
#define MSR_TEMPERATURE_TARGET      0x000001A2

/* EFER bits */
#define EFER_SCE                    (1ULL <<  0)  /* SYSCALL/SYSRET enable  */
#define EFER_LME                    (1ULL <<  8)  /* Long Mode Enable       */
#define EFER_LMA                    (1ULL << 10)  /* Long Mode Active       */
#define EFER_NXE                    (1ULL << 11)  /* No-Execute Enable      */

#endif /* ARCH_X86_64_MSR_H */
