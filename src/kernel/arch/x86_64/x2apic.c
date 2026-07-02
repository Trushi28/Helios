/**
 * @file x2apic.c
 * @brief x2APIC initialization, timer configuration, and IPI delivery.
 *
 * Enables x2APIC mode via MSR_IA32_APIC_BASE, configures the spurious
 * interrupt vector register, masks unused LVT entries, and provides
 * timer arming and IPI send functions.
 */

#include <arch/x86_64/x2apic.h>
#include <arch/x86_64/cpuid.h>
#include <arch/x86_64/tsc.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern NORETURN void panic(const char *msg);

/* TSC-Deadline mode support flag (per-core, but all cores on the same
 * CPU model have the same capability) */
static bool g_tsc_deadline_supported = false;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  x2APIC enable                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

void x2apic_init(void) {
    /* Verify x2APIC support */
    cpuid_result_t r = cpuid(1);
    if (!(r.ecx & CPUID_1_ECX_X2APIC)) {
        panic("x2APIC: not supported by CPU");
    }

    /* Check for TSC-Deadline mode support (CPUID.01H:ECX.TSC_Deadline[bit 24]) */
    g_tsc_deadline_supported = !!(r.ecx & (1u << 24));

    /* Enable x2APIC mode:
     * Set both global enable (bit 11) and x2APIC enable (bit 10) in
     * IA32_APIC_BASE MSR. */
    uint64_t apic_base = rdmsr(MSR_IA32_APIC_BASE);
    apic_base |= APIC_BASE_GLOBAL_ENABLE | APIC_BASE_X2APIC_ENABLE;
    wrmsr(MSR_IA32_APIC_BASE, apic_base);

    /* Set spurious interrupt vector and enable APIC (bit 8) */
    wrmsr(MSR_X2APIC_SIVR, APIC_SIVR_ENABLE | SPURIOUS_VECTOR);

    /* Mask all LVT entries except timer */
    wrmsr(MSR_X2APIC_LVT_LINT0,   APIC_LVT_MASKED);
    wrmsr(MSR_X2APIC_LVT_LINT1,   APIC_LVT_MASKED);
    wrmsr(MSR_X2APIC_LVT_ERROR,   APIC_LVT_MASKED);
    wrmsr(MSR_X2APIC_LVT_PMC,     APIC_LVT_MASKED);
    wrmsr(MSR_X2APIC_LVT_THERMAL, APIC_LVT_MASKED);

    /* Configure timer LVT — initially masked, armed by x2apic_arm_timer */
    if (g_tsc_deadline_supported) {
        /* TSC-Deadline mode: timer mode bits = 10b (bits 17:18) */
        wrmsr(MSR_X2APIC_LVT_TIMER,
              TIMER_VECTOR | (2u << 17));  /* mode=TSC-Deadline */
    } else {
        /* One-shot mode: timer mode bits = 00b */
        wrmsr(MSR_X2APIC_LVT_TIMER,
              TIMER_VECTOR | APIC_LVT_MASKED);  /* masked until armed */
    }

    /* Set TPR to 0 — accept all interrupts */
    wrmsr(MSR_X2APIC_TPR, 0);

    /* Clear any pending errors */
    wrmsr(MSR_X2APIC_ESR, 0);

    uint32_t id = x2apic_get_id();
    serial_printf("  x2APIC: enabled, ID=%u, TSC-Deadline=%s\n",
                  id, g_tsc_deadline_supported ? "yes" : "no");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  ID query                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

uint32_t x2apic_get_id(void) {
    return (uint32_t)rdmsr(MSR_X2APIC_ID);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Timer arming                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

void x2apic_arm_timer(uint64_t ns) {
    if (g_tsc_deadline_supported) {
        /* Write absolute TSC deadline */
        uint64_t deadline = rdtsc() + ns_to_tsc(ns);
        wrmsr(MSR_X2APIC_LVT_TIMER,
              TIMER_VECTOR | (2u << 17));  /* ensure TSC-Deadline mode, unmasked */
        wrmsr(0x6E0, deadline);  /* IA32_TSC_DEADLINE MSR = 0x6E0 */
    } else {
        /* One-shot mode using APIC timer.
         * APIC timer counts at bus frequency, which we approximate
         * from the TSC. Divide by 16 for a reasonable count range. */
        wrmsr(MSR_X2APIC_TIMER_DIV, 0x03);  /* divide by 16 */

        /* Convert ns to APIC timer ticks (approximate).
         * APIC timer freq ≈ bus_freq. For QEMU, bus_freq ≈ 1 GHz.
         * We use TSC as proxy divided by 16. */
        uint64_t tsc_ticks = ns_to_tsc(ns);
        uint32_t apic_ticks = (uint32_t)(tsc_ticks / 16);
        if (apic_ticks == 0) apic_ticks = 1;

        /* Unmask and set initial count */
        wrmsr(MSR_X2APIC_LVT_TIMER, TIMER_VECTOR);  /* one-shot, unmasked */
        wrmsr(MSR_X2APIC_TIMER_INIT, apic_ticks);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  IPI delivery                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

void x2apic_send_ipi(uint32_t dest_apic_id, uint8_t vector) {
    /* x2APIC ICR: bits [7:0] = vector, bits [10:8] = delivery mode (000=fixed),
     * bits [63:32] = destination APIC ID.
     * In x2APIC mode, ICR is a single 64-bit MSR write. */
    uint64_t icr = ((uint64_t)dest_apic_id << 32) |
                   ((uint64_t)APIC_DELIVERY_FIXED << 8) |
                   vector;
    wrmsr(MSR_X2APIC_ICR, icr);
}

void x2apic_send_init(uint32_t dest_apic_id) {
    /* INIT IPI: delivery mode = 101b = INIT, vector = 0 */
    uint64_t icr = ((uint64_t)dest_apic_id << 32) |
                   ((uint64_t)APIC_DELIVERY_INIT << 8);
    wrmsr(MSR_X2APIC_ICR, icr);
}

void x2apic_send_sipi(uint32_t dest_apic_id, uint8_t page) {
    /* SIPI: delivery mode = 110b = Startup, vector = page number */
    uint64_t icr = ((uint64_t)dest_apic_id << 32) |
                   ((uint64_t)APIC_DELIVERY_STARTUP << 8) |
                   page;
    wrmsr(MSR_X2APIC_ICR, icr);
}
