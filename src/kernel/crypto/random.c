/**
 * @file random.c
 * @brief Helios OS — Hardware entropy via RDRAND with TSC-LCG fallback.
 *
 * Uses x86 RDRAND instruction when available (CPUID leaf 1 ECX bit 30).
 * Falls back to a TSC-seeded linear congruential generator if RDRAND
 * is unavailable or repeatedly fails.
 */

#include <helios/types.h>
#include <arch/x86_64/cpuid.h>

/* Forward declarations */
extern void serial_puts(const char *s);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  RDRAND support detection                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

static bool g_rdrand_available = false;
static bool g_rdrand_checked   = false;

static void check_rdrand(void) {
    if (g_rdrand_checked) return;
    cpuid_result_t r = cpuid(1);
    g_rdrand_available = (r.ecx & CPUID_1_ECX_RDRAND) != 0;
    g_rdrand_checked = true;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  RDRAND wrapper                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

bool rdrand_u64(uint64_t *out) {
    check_rdrand();
    if (!g_rdrand_available) {
        return false;
    }

    /* Retry up to 10 times per Intel recommendation */
    for (int retry = 0; retry < 10; retry++) {
        uint64_t val;
        uint8_t  ok;
        __asm__ volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(val), "=qm"(ok)
        );
        if (ok) {
            *out = val;
            return true;
        }
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  TSC-seeded LCG fallback                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* Knuth LCG constants for 64-bit */
static uint64_t g_lcg_state = 0;

static uint64_t lcg_next(void) {
    if (g_lcg_state == 0) {
        /* Seed from TSC */
        g_lcg_state = rdtsc() ^ 0xDEADBEEFCAFEBABEULL;
    }
    g_lcg_state = g_lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return g_lcg_state;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

bool random_read(void *buf, size_t n_bytes) {
    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = n_bytes;

    check_rdrand();

    while (remaining > 0) {
        uint64_t val;
        bool hw_ok = false;

        if (g_rdrand_available) {
            hw_ok = rdrand_u64(&val);
        }
        if (!hw_ok) {
            /* Fallback to LCG */
            val = lcg_next();
        }

        size_t to_copy = remaining < 8 ? remaining : 8;
        for (size_t i = 0; i < to_copy; i++) {
            dst[i] = (uint8_t)(val >> (i * 8));
        }
        dst += to_copy;
        remaining -= to_copy;
    }

    return g_rdrand_available;
}
