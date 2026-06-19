/**
 * @file cpuid.h
 * @brief x86-64 CPUID wrapper functions for feature detection.
 */

#ifndef ARCH_X86_64_CPUID_H
#define ARCH_X86_64_CPUID_H

#include <helios/types.h>

typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} cpuid_result_t;

static ALWAYS_INLINE cpuid_result_t cpuid(uint32_t leaf) {
    cpuid_result_t r;
    __asm__ volatile("cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(0));
    return r;
}

static ALWAYS_INLINE cpuid_result_t cpuid_count(uint32_t leaf, uint32_t sub) {
    cpuid_result_t r;
    __asm__ volatile("cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(sub));
    return r;
}

/* Feature detection bits — CPUID leaf 1 */
#define CPUID_1_ECX_SSE3        (1U <<  0)
#define CPUID_1_ECX_SSE41       (1U << 19)
#define CPUID_1_ECX_SSE42       (1U << 20)
#define CPUID_1_ECX_X2APIC      (1U << 21)
#define CPUID_1_ECX_AVX         (1U << 28)
#define CPUID_1_ECX_RDRAND      (1U << 30)

#define CPUID_1_EDX_FPU         (1U <<  0)
#define CPUID_1_EDX_MSR         (1U <<  5)
#define CPUID_1_EDX_APIC        (1U <<  9)
#define CPUID_1_EDX_SSE         (1U << 25)
#define CPUID_1_EDX_SSE2        (1U << 26)

/* CPUID leaf 7 sub-leaf 0 */
#define CPUID_7_EBX_AVX2        (1U <<  5)
#define CPUID_7_EBX_SMEP        (1U <<  7)
#define CPUID_7_EBX_AVX512F     (1U << 16)
#define CPUID_7_EBX_RDSEED      (1U << 18)
#define CPUID_7_EBX_SMAP        (1U << 20)
#define CPUID_7_ECX_LA57        (1U << 16)

/* CPUID extended leaf 0x80000007 */
#define CPUID_80000007_EDX_INVARIANT_TSC  (1U << 8)

#endif /* ARCH_X86_64_CPUID_H */
