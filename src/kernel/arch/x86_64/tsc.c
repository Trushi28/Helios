/**
 * @file tsc.c
 * @brief TSC calibration — CPUID 0x15 with PIT channel 2 fallback.
 *
 * Method 1: CPUID leaf 0x15 gives TSC/crystal ratio + crystal frequency.
 * Method 2: PIT channel 2 50ms gate measurement (fallback for older CPUs
 *           and VMs that don't report CPUID 0x15 crystal frequency).
 */

#include <arch/x86_64/tsc.h>
#include <arch/x86_64/cpuid.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Global TSC frequency                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

uint64_t g_tsc_freq_hz  = 0;
uint32_t g_tsc_freq_mhz = 0;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  PIT constants for channel 2 calibration                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define PIT_FREQ     1193182ULL   /* PIT oscillator frequency in Hz */
#define PIT_CH2_PORT 0x42
#define PIT_CMD_PORT 0x43
#define PIT_GATE_PORT 0x61

/* 50 ms calibration window */
#define CALIBRATE_MS 50
#define PIT_TICKS    ((PIT_FREQ * CALIBRATE_MS) / 1000)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Invariant TSC check                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

bool tsc_is_invariant(void) {
    cpuid_result_t r = cpuid(0x80000007);
    return !!(r.edx & CPUID_80000007_EDX_INVARIANT_TSC);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Method 1: CPUID leaf 0x15                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

static bool try_cpuid_0x15(void) {
    /* Check if leaf 0x15 is supported */
    cpuid_result_t max_leaf = cpuid(0);
    if (max_leaf.eax < 0x15) return false;

    cpuid_result_t r = cpuid(0x15);
    /* EAX = denominator, EBX = numerator, ECX = crystal freq (0 if unknown) */
    if (r.eax == 0 || r.ebx == 0) return false;

    uint64_t crystal_hz = r.ecx;
    if (crystal_hz == 0) {
        /* Some Intel CPUs don't report crystal frequency in ECX.
         * Try well-known crystal frequencies based on CPUID leaf 0x16. */
        cpuid_result_t max2 = cpuid(0);
        if (max2.eax >= 0x16) {
            cpuid_result_t r16 = cpuid(0x16);
            if (r16.eax != 0) {
                /* EAX = base frequency in MHz */
                g_tsc_freq_hz  = (uint64_t)r16.eax * 1000000ULL;
                g_tsc_freq_mhz = r16.eax;
                serial_printf("  TSC: CPUID 0x16 base freq = %u MHz\n",
                              g_tsc_freq_mhz);
                return true;
            }
        }
        return false;
    }

    g_tsc_freq_hz  = crystal_hz * (uint64_t)r.ebx / (uint64_t)r.eax;
    g_tsc_freq_mhz = (uint32_t)(g_tsc_freq_hz / 1000000ULL);
    serial_printf("  TSC: CPUID 0x15 crystal=%lu Hz, ratio=%u/%u\n",
                  crystal_hz, r.ebx, r.eax);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Method 2: PIT channel 2 calibration (50 ms)                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void pit_calibrate(void) {
    serial_puts("  TSC: PIT channel 2 calibration (50 ms)...\n");

    /* Disable speaker, enable gate for channel 2 */
    uint8_t gate = inb(PIT_GATE_PORT);
    gate = (gate & 0xFD) | 0x01;   /* bit 0 = gate, bit 1 = speaker off */
    outb(PIT_GATE_PORT, gate);

    /* Program PIT channel 2 for one-shot countdown */
    outb(PIT_CMD_PORT, 0xB0);  /* Channel 2, lobyte/hibyte, mode 0 (one-shot) */

    uint16_t count = (uint16_t)PIT_TICKS;
    outb(PIT_CH2_PORT, (uint8_t)(count & 0xFF));
    outb(PIT_CH2_PORT, (uint8_t)((count >> 8) & 0xFF));

    /* Reset the gate to start the countdown */
    gate = inb(PIT_GATE_PORT);
    gate &= 0xFE;
    outb(PIT_GATE_PORT, gate);
    gate |= 0x01;
    outb(PIT_GATE_PORT, gate);

    /* Sample TSC at start */
    uint64_t tsc_start = rdtsc();

    /* Wait for PIT channel 2 output to go high (bit 5 of port 0x61) */
    while (!(inb(PIT_GATE_PORT) & 0x20)) {
        cpu_pause();
    }

    uint64_t tsc_end = rdtsc();
    uint64_t tsc_delta = tsc_end - tsc_start;

    /* freq = delta_tsc / (CALIBRATE_MS / 1000) = delta_tsc * 1000 / CALIBRATE_MS */
    g_tsc_freq_hz  = tsc_delta * 1000 / CALIBRATE_MS;
    g_tsc_freq_mhz = (uint32_t)(g_tsc_freq_hz / 1000000ULL);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public calibration entry point                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

void tsc_calibrate(void) {
    if (!try_cpuid_0x15()) {
        pit_calibrate();
    }

    serial_printf("  TSC: frequency = %lu Hz (%u MHz)%s\n",
                  g_tsc_freq_hz, g_tsc_freq_mhz,
                  tsc_is_invariant() ? " [invariant]" : " [variant]");

    if (g_tsc_freq_hz == 0) {
        serial_puts("  TSC: WARNING — calibration failed, frequency is 0\n");
    }
}
