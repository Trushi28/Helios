/**
 * @file tsc.h
 * @brief TSC (Time Stamp Counter) calibration and conversion utilities.
 */

#ifndef ARCH_X86_64_TSC_H
#define ARCH_X86_64_TSC_H

#include <helios/types.h>

extern uint64_t g_tsc_freq_hz;    /**< TSC frequency in Hz */
extern uint32_t g_tsc_freq_mhz;   /**< TSC frequency in MHz (approx) */

/**
 * @brief Calibrate the TSC frequency.
 * Uses CPUID leaf 0x15 if available, otherwise PIT channel 2 fallback.
 */
void tsc_calibrate(void);

/**
 * @brief Check if the TSC is invariant (constant frequency).
 * @return true if CPUID 0x80000007.EDX bit 8 is set.
 */
bool tsc_is_invariant(void);

/**
 * @brief Convert TSC ticks to nanoseconds.
 */
static ALWAYS_INLINE uint64_t tsc_to_ns(uint64_t ticks) {
    /* ticks * 1e9 / freq — use 128-bit intermediate to avoid overflow.
     * Approximate: ticks / (freq / 1e9) = ticks / ticks_per_ns */
    extern uint64_t g_tsc_freq_hz;
    if (g_tsc_freq_hz == 0) return 0;
    return ticks * 1000000000ULL / g_tsc_freq_hz;
}

/**
 * @brief Convert nanoseconds to TSC ticks.
 */
static ALWAYS_INLINE uint64_t ns_to_tsc(uint64_t ns) {
    extern uint64_t g_tsc_freq_hz;
    return ns * g_tsc_freq_hz / 1000000000ULL;
}

#endif /* ARCH_X86_64_TSC_H */
