/**
 * @file x2apic.h
 * @brief x2APIC initialization, timer, and IPI interface.
 */

#ifndef ARCH_X86_64_X2APIC_H
#define ARCH_X86_64_X2APIC_H

#include <helios/types.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/msr.h>

/* Timer vector — fires at vector 0x20, handled by scheduler_preempt */
#define TIMER_VECTOR 0x20

/**
 * @brief Enable x2APIC mode on the current core and configure SIVR/LVTs.
 */
void x2apic_init(void);

/**
 * @brief Return the x2APIC ID of the current core.
 */
uint32_t x2apic_get_id(void);

/**
 * @brief Arm the APIC timer to fire after `ns` nanoseconds.
 * Uses TSC-Deadline mode if supported, otherwise one-shot.
 */
void x2apic_arm_timer(uint64_t ns);

/**
 * @brief Send end-of-interrupt to the local APIC.
 */
static ALWAYS_INLINE void x2apic_eoi(void) {
    wrmsr(MSR_X2APIC_EOI, 0);
}

/**
 * @brief Send an IPI to a specific core.
 */
void x2apic_send_ipi(uint32_t dest_apic_id, uint8_t vector);

/**
 * @brief Send INIT IPI to a core.
 */
void x2apic_send_init(uint32_t dest_apic_id);

/**
 * @brief Send SIPI (Startup IPI) to a core.
 * @param page  SIPI vector — physical page number (address >> 12).
 */
void x2apic_send_sipi(uint32_t dest_apic_id, uint8_t page);

#endif /* ARCH_X86_64_X2APIC_H */
