/**
 * @file smp.h
 * @brief SMP (Symmetric Multi-Processing) initialization — AP bringup.
 */

#ifndef HELIOS_SMP_H
#define HELIOS_SMP_H

#include <helios/types.h>

/* Trampoline destination — physical page 0x8000 (below 1 MiB, already reserved).
 * SIPI vector = 0x08 (= 0x8000 >> 12). No PMM allocation needed. */
#define AP_TRAMPOLINE_PHYS  0x8000ULL
#define AP_SIPI_VECTOR      0x08

/* Data block offset from trampoline base — must match ap_trampoline.asm */
#define AP_DATA_OFFSET      0x0F00

/* IPI vector numbers */
#define IPI_RESCHEDULE      0xF0
#define IPI_TLB_SHOOTDOWN   0xF1
#define IPI_HALT            0xF2
#define IPI_PANIC           0xF3

/**
 * @brief Boot all Application Processors discovered via MADT.
 *
 * Copies the AP trampoline to physical 0x8000, sends INIT-SIPI-SIPI
 * to each AP, and waits for them to signal readiness.
 */
void smp_init(void);

#endif /* HELIOS_SMP_H */
