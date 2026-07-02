/**
 * @file madt.h
 * @brief ACPI MADT (Multiple APIC Description Table) parsing.
 *
 * Parses the RSDP → XSDT → MADT chain to discover CPUs, I/O APICs,
 * and interrupt source overrides. Populates global tables used by
 * x2APIC init and SMP bringup.
 */

#ifndef HELIOS_ACPI_MADT_H
#define HELIOS_ACPI_MADT_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  CPU info table                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define HELIOS_MAX_CPUS 256

typedef struct {
    uint32_t x2apic_id;
    uint32_t acpi_uid;
    bool     enabled;
    bool     is_bsp;
    bool     online;         /* set to true once AP signals ready */
} helios_cpu_info_t;

extern helios_cpu_info_t g_cpu_table[HELIOS_MAX_CPUS];
extern uint32_t          g_cpu_count;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  I/O APIC info table                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define HELIOS_MAX_IOAPICS 4

typedef struct {
    uint32_t id;
    uint32_t gsi_base;
    uint64_t address;
} ioapic_info_t;

extern ioapic_info_t g_ioapic_table[HELIOS_MAX_IOAPICS];
extern uint32_t      g_ioapic_count;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  IRQ source overrides                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define HELIOS_MAX_IRQ_OVERRIDES 16

typedef struct {
    uint8_t  bus;
    uint8_t  source;        /* ISA IRQ */
    uint32_t gsi;           /* Global System Interrupt */
    uint16_t flags;         /* Polarity / trigger mode */
} irq_override_t;

extern irq_override_t g_irq_overrides[HELIOS_MAX_IRQ_OVERRIDES];
extern uint32_t        g_irq_override_count;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  API                                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parse RSDP → XSDT → MADT to populate CPU, I/O APIC, and IRQ tables.
 * @param rsdp_phys  Physical address of the ACPI RSDP (from boot_info).
 */
void madt_parse(uint64_t rsdp_phys);

/**
 * @brief Mark the BSP entry in g_cpu_table.
 *
 * Must be called after x2apic_init() so x2apic_get_id() returns
 * the BSP's actual APIC ID.
 */
void madt_mark_bsp(void);

#endif /* HELIOS_ACPI_MADT_H */
