/**
 * @file madt.c
 * @brief ACPI MADT parsing — discovers CPUs, I/O APICs, and IRQ overrides.
 *
 * Walks RSDP → XSDT → MADT and populates the global tables declared
 * in madt.h. Handles both legacy Local APIC (type 0x00) and x2APIC
 * (type 0x09) entries.
 */

#include <helios/acpi/madt.h>
#include <arch/x86_64/paging.h>
#include <helios/types.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern NORETURN void panic(const char *msg);
extern void *memcpy(void *dest, const void *src, size_t n);
extern int memcmp(const void *a, const void *b, size_t n);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Global tables                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

helios_cpu_info_t g_cpu_table[HELIOS_MAX_CPUS];
uint32_t          g_cpu_count = 0;

ioapic_info_t     g_ioapic_table[HELIOS_MAX_IOAPICS];
uint32_t          g_ioapic_count = 0;

irq_override_t    g_irq_overrides[HELIOS_MAX_IRQ_OVERRIDES];
uint32_t          g_irq_override_count = 0;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  ACPI table structures                                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct PACKED {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* ACPI 2.0+ fields */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

typedef struct PACKED {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct PACKED {
    acpi_sdt_header_t header;
    uint32_t          local_apic_addr;
    uint32_t          flags;
} acpi_madt_t;

/* MADT entry header */
typedef struct PACKED {
    uint8_t type;
    uint8_t length;
} madt_entry_header_t;

/* Type 0x00: Processor Local APIC */
typedef struct PACKED {
    madt_entry_header_t header;
    uint8_t  acpi_uid;
    uint8_t  apic_id;
    uint32_t flags;
} madt_local_apic_t;

/* Type 0x01: I/O APIC */
typedef struct PACKED {
    madt_entry_header_t header;
    uint8_t  id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} madt_ioapic_t;

/* Type 0x02: Interrupt Source Override */
typedef struct PACKED {
    madt_entry_header_t header;
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
} madt_irq_override_t;

/* Type 0x09: Processor Local x2APIC */
typedef struct PACKED {
    madt_entry_header_t header;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_uid;
} madt_x2apic_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Helpers                                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

static inline void *acpi_phys_to_virt(uint64_t phys) {
    return (void *)(KERNEL_PHYS_MAP_BASE + phys);
}

/**
 * @brief Verify ACPI table checksum: sum of all bytes must be 0 mod 256.
 * @return true if checksum is valid.
 */
static bool acpi_verify_checksum(const void *table, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return (sum == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  MADT parsing                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

void madt_parse(uint64_t rsdp_phys) {
    serial_puts("  MADT: parsing ACPI tables...\n");

    /* ── Validate RSDP ────────────────────────────────────────────────── */
    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)acpi_phys_to_virt(rsdp_phys);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        panic("MADT: invalid RSDP signature");
    }
    if (rsdp->revision < 2) {
        panic("MADT: ACPI revision < 2.0 — XSDT not available");
    }
    if (!acpi_verify_checksum(rsdp, rsdp->length)) {
        panic("MADT: RSDP checksum failed");
    }

    /* ── Walk XSDT to find MADT ──────────────────────────────────────── */
    const acpi_sdt_header_t *xsdt =
        (const acpi_sdt_header_t *)acpi_phys_to_virt(rsdp->xsdt_address);
    if (memcmp(xsdt->signature, "XSDT", 4) != 0) {
        panic("MADT: invalid XSDT signature");
    }
    if (!acpi_verify_checksum(xsdt, xsdt->length)) {
        panic("MADT: XSDT checksum failed");
    }

    uint32_t entry_count = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    const uint64_t *xsdt_entries =
        (const uint64_t *)((const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));

    const acpi_madt_t *madt = NULL;
    for (uint32_t i = 0; i < entry_count; i++) {
        const acpi_sdt_header_t *hdr =
            (const acpi_sdt_header_t *)acpi_phys_to_virt(xsdt_entries[i]);
        if (memcmp(hdr->signature, "APIC", 4) == 0) {
            if (!acpi_verify_checksum(hdr, hdr->length)) {
                panic("MADT: MADT checksum failed");
            }
            madt = (const acpi_madt_t *)hdr;
            break;
        }
    }
    if (!madt) {
        panic("MADT: no MADT found in XSDT");
    }

    serial_printf("  MADT: found at phys 0x%lx, length %u\n",
                  (uint64_t)madt - KERNEL_PHYS_MAP_BASE, madt->header.length);

    /* ── Walk MADT entries ────────────────────────────────────────────── */
    const uint8_t *ptr = (const uint8_t *)madt + sizeof(acpi_madt_t);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (ptr + 2 <= end) {
        const madt_entry_header_t *eh = (const madt_entry_header_t *)ptr;
        if (eh->length < 2 || ptr + eh->length > end) break;

        switch (eh->type) {
        case 0x00: { /* Processor Local APIC */
            const madt_local_apic_t *la = (const madt_local_apic_t *)eh;
            bool enabled = (la->flags & 1) || (la->flags & 2);
            if (enabled && g_cpu_count < HELIOS_MAX_CPUS) {
                g_cpu_table[g_cpu_count].x2apic_id = la->apic_id;
                g_cpu_table[g_cpu_count].acpi_uid  = la->acpi_uid;
                g_cpu_table[g_cpu_count].enabled    = true;
                g_cpu_table[g_cpu_count].is_bsp     = false;
                g_cpu_table[g_cpu_count].online      = false;
                g_cpu_count++;
            }
            break;
        }
        case 0x01: { /* I/O APIC */
            const madt_ioapic_t *io = (const madt_ioapic_t *)eh;
            if (g_ioapic_count < HELIOS_MAX_IOAPICS) {
                g_ioapic_table[g_ioapic_count].id       = io->id;
                g_ioapic_table[g_ioapic_count].address   = io->address;
                g_ioapic_table[g_ioapic_count].gsi_base  = io->gsi_base;
                g_ioapic_count++;
            }
            break;
        }
        case 0x02: { /* Interrupt Source Override */
            const madt_irq_override_t *ov = (const madt_irq_override_t *)eh;
            if (g_irq_override_count < HELIOS_MAX_IRQ_OVERRIDES) {
                g_irq_overrides[g_irq_override_count].bus    = ov->bus;
                g_irq_overrides[g_irq_override_count].source = ov->source;
                g_irq_overrides[g_irq_override_count].gsi    = ov->gsi;
                g_irq_overrides[g_irq_override_count].flags  = ov->flags;
                g_irq_override_count++;
            }
            break;
        }
        case 0x09: { /* Processor Local x2APIC */
            const madt_x2apic_t *x2 = (const madt_x2apic_t *)eh;
            bool enabled = (x2->flags & 1) || (x2->flags & 2);
            if (enabled && g_cpu_count < HELIOS_MAX_CPUS) {
                g_cpu_table[g_cpu_count].x2apic_id = x2->x2apic_id;
                g_cpu_table[g_cpu_count].acpi_uid  = x2->acpi_uid;
                g_cpu_table[g_cpu_count].enabled    = true;
                g_cpu_table[g_cpu_count].is_bsp     = false;
                g_cpu_table[g_cpu_count].online      = false;
                g_cpu_count++;
            }
            break;
        }
        default:
            /* Unknown MADT entry type — skip */
            break;
        }

        ptr += eh->length;
    }

    serial_printf("  MADT: %u CPUs, %u I/O APICs, %u IRQ overrides\n",
                  g_cpu_count, g_ioapic_count, g_irq_override_count);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  BSP marking                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

void madt_mark_bsp(void) {
    /* x2apic_get_id() returns the BSP's x2APIC ID. We import it
     * via a forward declaration to avoid a circular header dependency. */
    extern uint32_t x2apic_get_id(void);
    uint32_t bsp_id = x2apic_get_id();

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpu_table[i].x2apic_id == bsp_id) {
            g_cpu_table[i].is_bsp = true;
            g_cpu_table[i].online = true;
            serial_printf("  MADT: BSP is CPU[%u], x2APIC ID %u\n", i, bsp_id);
            return;
        }
    }
    serial_printf("  MADT: WARNING — BSP x2APIC ID %u not found in table\n",
                  bsp_id);
}
