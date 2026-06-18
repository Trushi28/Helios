# 13 — ACPI Table Parsing & Power Management

> **Subsystem:** ACPI / Power  
> **Owner:** Kernel team  
> **Dependencies:** UEFI RSDP, physical memory map  
> **Related:** [01-BOOT.md](./01-BOOT.md), [03-SCHEDULER.md](./03-SCHEDULER.md), [14-INTERRUPTS.md](./14-INTERRUPTS.md)

---

## 1. ACPI Overview

ACPI (Advanced Configuration and Power Interface) provides the kernel with machine-readable hardware topology, interrupt routing, power management, and thermal control. Helios parses ACPI tables during early boot and uses them throughout the kernel's lifetime.

---

## 2. Table Discovery

### 2.1 RSDP → XSDT Traversal

The bootloader locates the RSDP via the UEFI Configuration Table. The kernel follows the pointer chain:

```c
typedef struct {
    char        signature[8];       // "RSD PTR "
    uint8_t     checksum;
    char        oem_id[6];
    uint8_t     revision;           // 2 for ACPI 2.0+
    uint32_t    rsdt_address;       // 32-bit RSDT (ACPI 1.0, ignored)
    uint32_t    length;
    uint64_t    xsdt_address;       // 64-bit XSDT (ACPI 2.0+, used)
    uint8_t     extended_checksum;
    uint8_t     _reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    acpi_sdt_header_t header;       // Signature = "XSDT"
    uint64_t          entries[];    // Array of 64-bit physical pointers to other tables
} __attribute__((packed)) acpi_xsdt_t;

typedef struct {
    char        signature[4];
    uint32_t    length;
    uint8_t     revision;
    uint8_t     checksum;
    char        oem_id[6];
    char        oem_table_id[8];
    uint32_t    oem_revision;
    uint32_t    creator_id;
    uint32_t    creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;
```

### 2.2 Table Registry

The kernel builds a registry of all discovered ACPI tables:

```c
#define ACPI_MAX_TABLES 32

typedef struct {
    char                signature[4];
    acpi_sdt_header_t  *header;         // Mapped virtual address
    uint64_t            phys_addr;
    uint32_t            length;
} acpi_table_entry_t;

extern acpi_table_entry_t g_acpi_tables[ACPI_MAX_TABLES];
extern uint32_t           g_acpi_table_count;

// Find a table by its 4-character signature
acpi_sdt_header_t *acpi_find_table(const char signature[4]);
```

---

## 3. Critical Tables

### 3.1 MADT (Multiple APIC Description Table)

Documented in [01-BOOT.md](./01-BOOT.md). Provides:
- Local APIC / x2APIC enumeration for SMP
- I/O APIC addresses
- Interrupt source overrides (ISA → GSI remapping)
- NMI configuration

### 3.2 MCFG (Memory-Mapped Configuration Space)

Documented in [07-DRIVERS.md](./07-DRIVERS.md). Provides PCIe ECAM base addresses.

### 3.3 FADT (Fixed ACPI Description Table)

```c
typedef struct {
    acpi_sdt_header_t header;
    uint32_t    firmware_ctrl;      // Physical address of FACS
    uint32_t    dsdt;               // Physical address of DSDT
    uint8_t     _reserved1;
    uint8_t     preferred_pm_profile; // 0=unspec, 1=desktop, 2=mobile, 3=workstation
    uint16_t    sci_interrupt;      // SCI interrupt vector
    uint32_t    smi_cmd;            // SMI command port
    uint8_t     acpi_enable;
    uint8_t     acpi_disable;
    uint8_t     s4bios_req;
    uint8_t     pstate_cnt;
    uint32_t    pm1a_evt_blk;
    uint32_t    pm1b_evt_blk;
    uint32_t    pm1a_cnt_blk;
    uint32_t    pm1b_cnt_blk;
    uint32_t    pm2_cnt_blk;
    uint32_t    pm_tmr_blk;
    uint32_t    gpe0_blk;
    uint32_t    gpe1_blk;
    uint8_t     pm1_evt_len;
    uint8_t     pm1_cnt_len;
    uint8_t     pm2_cnt_len;
    uint8_t     pm_tmr_len;
    uint8_t     gpe0_blk_len;
    uint8_t     gpe1_blk_len;
    uint8_t     gpe1_base;
    uint8_t     cst_cnt;
    uint16_t    p_lvl2_lat;
    uint16_t    p_lvl3_lat;
    uint16_t    flush_size;
    uint16_t    flush_stride;
    uint8_t     duty_offset;
    uint8_t     duty_width;
    uint8_t     day_alarm;
    uint8_t     month_alarm;
    uint8_t     century;
    uint16_t    boot_arch_flags;
    uint8_t     _reserved2;
    uint32_t    flags;
    // ... ACPI 2.0+ extended fields with GenericAddress structures
    // x_pm1a_evt_blk, x_pm1b_evt_blk, etc.
    uint8_t     reset_reg[12];      // Generic Address Structure
    uint8_t     reset_value;
    uint16_t    arm_boot_arch;
    uint8_t     fadt_minor_version;
    uint64_t    x_firmware_ctrl;
    uint64_t    x_dsdt;
    // ... more extended GenericAddress fields
} __attribute__((packed)) acpi_fadt_t;
```

### 3.4 SRAT (System Resource Affinity Table)

Provides NUMA topology — which CPU cores and memory ranges belong to which NUMA domain:

```c
typedef struct {
    uint8_t     type;               // 0 = Processor Affinity, 1 = Memory Affinity
    uint8_t     length;
    union {
        struct {                    // Type 0: Processor → NUMA domain
            uint8_t     proximity_domain_lo;
            uint8_t     apic_id;
            uint32_t    flags;      // bit 0: enabled
            uint8_t     sapic_eid;
            uint8_t     proximity_domain_hi[3];
            uint32_t    clock_domain;
        } processor;
        struct {                    // Type 1: Memory range → NUMA domain
            uint32_t    proximity_domain;
            uint16_t    _reserved;
            uint64_t    base_address;
            uint64_t    length;
            uint32_t    _reserved2;
            uint32_t    flags;      // bit 0: enabled, bit 1: hot-pluggable
            uint64_t    _reserved3;
        } memory;
    };
} __attribute__((packed)) srat_entry_t;

// Parsed NUMA topology
typedef struct {
    uint32_t    domain_id;
    uint64_t    mem_base;
    uint64_t    mem_size;
    uint32_t    cpu_ids[HELIOS_MAX_CPUS];
    uint32_t    cpu_count;
} numa_domain_t;

#define NUMA_MAX_DOMAINS 16
extern numa_domain_t g_numa_domains[NUMA_MAX_DOMAINS];
extern uint32_t      g_numa_domain_count;
```

### 3.5 SLIT (System Locality Information Table)

Provides relative latency between NUMA domains:

```c
// SLIT contains an N×N matrix of relative distances
// Entry [i][j] = relative latency from domain i to domain j
// 10 = local, >10 = remote, 255 = unreachable
uint8_t g_numa_distances[NUMA_MAX_DOMAINS][NUMA_MAX_DOMAINS];
```

### 3.6 HPET (High Precision Event Timer)

```c
typedef struct {
    acpi_sdt_header_t header;
    uint32_t    event_timer_block_id;
    uint8_t     address_space_id;       // 0 = memory
    uint8_t     register_bit_width;
    uint8_t     register_bit_offset;
    uint8_t     _reserved;
    uint64_t    base_address;           // HPET MMIO base
    uint8_t     hpet_number;
    uint16_t    min_clock_ticks;
    uint8_t     page_protection;
} __attribute__((packed)) acpi_hpet_t;

void hpet_init(void);
uint64_t hpet_read_counter(void);
void hpet_set_oneshot(uint64_t deadline_ns);
```

### 3.7 DMAR (DMA Remapping)

Intel VT-d IOMMU enumeration:

```c
typedef struct {
    acpi_sdt_header_t header;
    uint8_t     host_addr_width;    // Physical address width - 1
    uint8_t     flags;
    uint8_t     _reserved[10];
    // Variable-length remapping structures follow
} __attribute__((packed)) acpi_dmar_t;

typedef struct {
    uint16_t    type;       // 0 = DRHD (DMA Remapping Hardware Unit)
    uint16_t    length;
    uint8_t     flags;      // bit 0: INCLUDE_PCI_ALL
    uint8_t     _reserved;
    uint16_t    segment;
    uint64_t    base_address;   // IOMMU register base
    // Device scope entries follow
} __attribute__((packed)) dmar_drhd_t;
```

---

## 4. Power Management

### 4.1 Sleep States

| State | Description | Implementation |
|-------|-------------|---------------|
| S0 | Working state | Normal operation |
| S1 | Power on suspend (CPU halted) | `HLT` instruction on all cores |
| S3 | Suspend to RAM | Save state, power down CPU/devices |
| S4 | Suspend to disk | Serialize graph state, shutdown |
| S5 | Soft off | Clean shutdown |

### 4.2 ACPI Shutdown

```c
void acpi_shutdown(void) {
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table("FACP");
    if (!fadt) {
        // Fallback: triple fault to reboot, or infinite HLT
        serial_puts("ACPI: No FADT, cannot shutdown\n");
        for (;;) asm volatile("hlt");
    }

    // Write SLP_TYP | SLP_EN to PM1a control register
    // SLP_TYP values come from _S5 object in DSDT (AML interpretation)
    uint16_t pm1a = fadt->pm1a_cnt_blk;
    outw(pm1a, (SLP_TYP_S5 << 10) | SLP_EN);

    // If PM1b exists, write there too
    if (fadt->pm1b_cnt_blk) {
        outw(fadt->pm1b_cnt_blk, (SLP_TYP_S5 << 10) | SLP_EN);
    }

    // Should not reach here
    for (;;) asm volatile("hlt");
}
```

### 4.3 ACPI Reboot

```c
void acpi_reboot(void) {
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table("FACP");

    // Method 1: FADT reset register (ACPI 2.0+)
    if (fadt && (fadt->flags & FADT_RESET_REG_SUP)) {
        // Write reset_value to the Generic Address specified in reset_reg
        acpi_write_gas(&fadt->reset_reg, fadt->reset_value);
    }

    // Method 2: Keyboard controller reset
    outb(0x64, 0xFE);

    // Method 3: Triple fault
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    asm volatile("lidt %0; int3" :: "m"(null_idt));
}
```

### 4.4 CPU Power States (C-States)

The scheduler's idle loop uses C-states for power efficiency:

```c
void scheduler_idle_loop(void) {
    for (;;) {
        if (scheduler_has_work(this_core_sched())) {
            scheduler_dispatch(this_core_sched());
        } else {
            // Enter MWAIT for efficient idle (if supported)
            if (cpu_supports_mwait()) {
                // MONITOR/MWAIT: wake on write to monitored address
                uint64_t *monitor_addr = &this_core_sched()->priority_bitmap;
                asm volatile("monitor" :: "a"(monitor_addr), "c"(0), "d"(0));
                asm volatile("mwait" :: "a"(0x20), "c"(0));  // C1E hint
            } else {
                asm volatile("hlt");
            }
        }
    }
}
```

### 4.5 Thermal Management

```c
// Thermal zone monitoring (from ACPI _TMP method or MSR)
typedef struct {
    uint32_t    current_temp_dK;    // Temperature in deci-Kelvin
    uint32_t    trip_passive_dK;    // Passive cooling threshold
    uint32_t    trip_critical_dK;   // Critical shutdown threshold
    uint32_t    throttle_pct;       // Current throttle percentage (0–100)
} thermal_zone_t;

// CPU thermal monitoring via MSR (Intel: MSR_THERM_STATUS)
uint32_t cpu_get_temperature(uint32_t core_id) {
    uint64_t therm = rdmsr(MSR_IA32_THERM_STATUS);
    uint64_t tjmax = rdmsr(MSR_TEMPERATURE_TARGET);
    uint32_t tj_max = (tjmax >> 16) & 0xFF;
    uint32_t digital_readout = (therm >> 16) & 0x7F;
    return tj_max - digital_readout;  // Celsius
}

// Response actions
void thermal_check(void) {
    uint32_t temp = cpu_get_temperature(this_core_id());
    if (temp >= THERMAL_CRITICAL_C) {
        acpi_shutdown();
    } else if (temp >= THERMAL_THROTTLE_C) {
        // Reduce scheduler time slices, lower clock via P-states
        cpu_throttle(temp);
    }
}
```

---

## 5. Power Management Syscalls

```c
// Request system state change (requires elevated capability)
int sys_power(power_action_t action);

typedef enum {
    POWER_SHUTDOWN  = 0,
    POWER_REBOOT    = 1,
    POWER_SUSPEND   = 2,    // S3
    POWER_HIBERNATE = 3,    // S4
} power_action_t;

// Query thermal status
int sys_thermal_info(thermal_zone_t *out);

// Query battery status (for mobile platforms, future)
int sys_battery_info(battery_info_t *out);
```

---

*Next: [14-INTERRUPTS.md](./14-INTERRUPTS.md) — IDT, x2APIC & MSI/MSI-X Interrupt Architecture*
