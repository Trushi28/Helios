# 07 — Driver Isolation & PCIe Enumeration

> **Subsystem:** Driver Framework  
> **Owner:** Kernel team  
> **Dependencies:** PCIe ECAM (MCFG), IOMMU (DMAR), capability system, SASOS  
> **Related:** [01-BOOT.md](./01-BOOT.md), [02-MEMORY.md](./02-MEMORY.md), [04-STORAGE.md](./04-STORAGE.md), [14-INTERRUPTS.md](./14-INTERRUPTS.md)

---

## 1. Design Philosophy

Helios drivers are **not** kernel modules compiled into a monolithic binary. Every driver is an **isolated micro-program** running in user-space privilege (Ring 3), communicating with hardware exclusively through capability-mediated MMIO mappings and IOMMU-protected DMA windows.

| Traditional OS | Helios |
|---------------|--------|
| Kernel module, runs in Ring 0 | Micro-program, runs in Ring 3 |
| Crash in driver = kernel panic | Crash in driver = driver restart |
| Full access to all kernel memory | Capability-bounded MMIO + DMA only |
| No DMA protection (without custom IOMMU) | IOMMU enforced per-device |
| Loaded as `.ko` / `.sys` binary | Loaded as content-addressed object from graph store |

---

## 2. PCIe Enumeration

### 2.1 ECAM Discovery

PCIe Enhanced Configuration Access Mechanism (ECAM) replaces legacy I/O port-based PCI config space. The MCFG ACPI table provides the ECAM base address:

```c
typedef struct {
    uint64_t    ecam_base_phys;     // Physical base of ECAM region
    uint16_t    segment_group;       // PCI segment group
    uint8_t     start_bus;          // First bus in this segment
    uint8_t     end_bus;            // Last bus in this segment
} mcfg_entry_t;

// Parse MCFG table from ACPI
void pcie_parse_mcfg(acpi_table_t *mcfg);

// Map ECAM region into SASOS MMIO area
void pcie_map_ecam(mcfg_entry_t *entry);
```

### 2.2 ECAM Config Space Access

Each device function's 4 KiB configuration space is memory-mapped:

```c
// Calculate ECAM address for a specific BDF (Bus/Device/Function)
static inline volatile void *ecam_addr(uint16_t seg, uint8_t bus,
                                        uint8_t dev, uint8_t func,
                                        uint16_t offset) {
    uint64_t addr = g_ecam_base +
                    ((uint64_t)bus << 20) |
                    ((uint64_t)dev << 15) |
                    ((uint64_t)func << 12) |
                    offset;
    return (volatile void *)addr;
}

// Read/write PCIe config space
uint32_t pcie_config_read32(uint16_t seg, uint8_t bus, uint8_t dev,
                            uint8_t func, uint16_t offset);
void     pcie_config_write32(uint16_t seg, uint8_t bus, uint8_t dev,
                             uint8_t func, uint16_t offset, uint32_t value);
```

### 2.3 Device Discovery

```c
typedef struct pcie_device {
    uint16_t    segment;
    uint8_t     bus;
    uint8_t     device;
    uint8_t     function;

    uint16_t    vendor_id;
    uint16_t    device_id;
    uint16_t    subsystem_vendor_id;
    uint16_t    subsystem_device_id;
    uint8_t     class_code;
    uint8_t     subclass;
    uint8_t     prog_if;
    uint8_t     revision;
    uint8_t     header_type;
    uint8_t     interrupt_pin;
    uint8_t     interrupt_line;

    // BARs
    struct {
        uint64_t    phys_base;
        uint64_t    size;
        bool        is_mmio;        // true = MMIO, false = I/O port
        bool        is_64bit;
        bool        is_prefetchable;
    } bars[6];

    // MSI/MSI-X capabilities
    struct {
        bool        msi_capable;
        bool        msix_capable;
        uint16_t    msix_table_size;
        uint8_t     msix_table_bar;
        uint32_t    msix_table_offset;
        uint8_t     msix_pba_bar;
        uint32_t    msix_pba_offset;
    } interrupt_caps;

    // Power management
    uint8_t     pm_cap_offset;
    uint8_t     current_power_state;    // D0, D1, D2, D3

    // Driver binding
    uint32_t    driver_mprog_id;        // Bound driver micro-program (0 = unbound)
    cap_token_t driver_mmio_caps[6];    // MMIO capabilities issued to driver

    struct list_head list;              // Global device list link
} pcie_device_t;

// Global device registry
extern struct list_head g_pcie_devices;
extern uint32_t         g_pcie_device_count;

void pcie_enumerate(void) {
    for (uint16_t bus = 0; bus <= 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pcie_config_read16(0, bus, dev, func, 0x00);
                if (vendor == 0xFFFF) {
                    if (func == 0) break;  // No device, skip remaining functions
                    continue;
                }

                pcie_device_t *pdev = slab_alloc(&pcie_device_cache);
                pdev->segment = 0;
                pdev->bus = bus;
                pdev->device = dev;
                pdev->function = func;
                pdev->vendor_id = vendor;
                pdev->device_id = pcie_config_read16(0, bus, dev, func, 0x02);
                // ... populate remaining fields ...

                pcie_parse_bars(pdev);
                pcie_parse_capabilities(pdev);
                list_add_tail(&pdev->list, &g_pcie_devices);
                g_pcie_device_count++;

                // Check if multi-function device
                if (func == 0) {
                    uint8_t header = pcie_config_read8(0, bus, dev, 0, 0x0E);
                    if (!(header & 0x80)) break;  // Not multi-function
                }
            }
        }
    }
}
```

---

## 3. Driver Matching & Binding

### 3.1 Driver Manifest

Each driver is an object in the graph store with a machine-readable manifest:

```c
typedef struct {
    // Match criteria (any of these can match)
    struct {
        uint16_t    vendor_id;          // 0xFFFF = wildcard
        uint16_t    device_id;          // 0xFFFF = wildcard
        uint8_t     class_code;         // 0xFF = wildcard
        uint8_t     subclass;           // 0xFF = wildcard
        uint8_t     prog_if;            // 0xFF = wildcard
    } match_rules[16];
    uint32_t    match_rule_count;

    // Driver metadata
    char        name[64];               // "nvme", "xhci", "virtio-gpu", etc.
    char        author[64];
    uint32_t    version;
    object_id_t code_oid;               // Object containing driver executable
    uint32_t    required_caps;          // CAP_PERM_DMA | CAP_PERM_NOCACHE | ...
    priority_t  priority;               // Scheduling priority for this driver
} driver_manifest_t;
```

### 3.2 Binding Process

```
1. PCIe enumeration discovers device
2. Kernel scans graph store for driver manifests matching the device
3. Best-matching driver manifest is selected
4. Kernel spawns driver micro-program from the manifest's code object
5. Kernel maps device BARs into SASOS, creates MMIO capability tokens
6. Kernel sets up IOMMU domain for the device
7. Kernel allocates DMA-capable memory region, issues DMA capability
8. Kernel configures MSI-X vectors targeting the driver's IRQ handler
9. Kernel passes capabilities + device info to driver via IPC
10. Driver initializes hardware using its capability-bounded MMIO access
```

```c
int driver_bind(pcie_device_t *pdev, driver_manifest_t *manifest) {
    // 1. Spawn driver micro-program
    cap_token_t code_cap = objstore_load_executable(manifest->code_oid);
    uint32_t mprog_id = sys_spawn(&code_cap, NULL,
                                   manifest->entry_point,
                                   manifest->priority,
                                   manifest->name);

    // 2. Map BARs and issue MMIO capabilities
    for (int i = 0; i < 6; i++) {
        if (pdev->bars[i].phys_base == 0) continue;
        pdev->driver_mmio_caps[i] = mmio_map_bar(pdev, i, mprog_id);
    }

    // 3. Set up IOMMU
    cap_token_t dma_cap = sys_cap_alloc(DMA_REGION_SIZE,
                                         CAP_PERM_READ | CAP_PERM_WRITE | CAP_PERM_DMA,
                                         ALLOC_CONTIGUOUS | ALLOC_PINNED);
    iommu_setup_device(pdev, &dma_cap);

    // 4. Configure MSI-X
    uint8_t vector = irq_alloc_vector();
    msix_configure(pdev, 0, vector, mprog_id);

    // 5. Send init message to driver via IPC
    driver_init_msg_t msg = {
        .device = *pdev,
        .mmio_caps = pdev->driver_mmio_caps,
        .dma_cap = dma_cap,
        .irq_vector = vector,
    };
    ipc_send(mprog_id, &msg, sizeof(msg));

    pdev->driver_mprog_id = mprog_id;
    return 0;
}
```

---

## 4. Driver Crash Recovery

Because drivers run as isolated micro-programs, a driver crash does not bring down the kernel:

```
Driver crash detected (segfault, capability violation, watchdog timeout)
  │
  ├─ 1. Kernel receives micro-program death notification
  ├─ 2. Revoke all capabilities held by the dead driver
  ├─ 3. Tear down IOMMU domain (stops DMA immediately)
  ├─ 4. Reset the PCIe device (Function Level Reset if supported)
  ├─ 5. Unmap MMIO BARs
  ├─ 6. Log crash report to graph store
  ├─ 7. Re-enumerate the device
  └─ 8. Re-bind and respawn the driver micro-program
```

```c
void driver_handle_crash(uint32_t mprog_id) {
    pcie_device_t *pdev = driver_find_device(mprog_id);
    if (!pdev) return;

    // Revoke all capabilities
    cap_revoke_all(mprog_id);

    // Tear down IOMMU
    iommu_detach_device(pdev);

    // Function Level Reset
    if (pcie_supports_flr(pdev)) {
        pcie_function_level_reset(pdev);
    } else {
        pcie_secondary_bus_reset(pdev);
    }

    // Unbind
    pdev->driver_mprog_id = 0;

    // Log
    log_driver_crash(pdev, mprog_id);

    // Re-bind after delay (exponential backoff on repeated crashes)
    scheduler_delayed_call(driver_rebind, pdev, restart_delay_ms(pdev));
}
```

---

## 5. Driver API

Drivers interact with the kernel through a minimal syscall interface:

```c
// ── MMIO Access (via capability-bounded pointers) ──
// Drivers read/write MMIO directly through their capability pointers.
// No syscall needed — capability bounds are enforced by the hardware/kernel.

// ── DMA ──
// Translate virtual address to physical (for programming DMA descriptors)
phys_addr_t sys_dma_virt_to_phys(cap_token_t *dma_cap, void *virt_addr);

// ── Interrupts ──
// Register an interrupt handler (receives IRQs via IPC)
int sys_irq_register(uint8_t vector, uint64_t ipc_port);

// Acknowledge an interrupt (re-enable delivery)
void sys_irq_ack(uint8_t vector);

// ── Device Power Management ──
int sys_device_set_power(pcie_bdf_t bdf, uint8_t power_state);  // D0–D3

// ── Bus Mastering ──
// Request the kernel to enable bus mastering for the device
int sys_device_enable_busmaster(pcie_bdf_t bdf);
```

---

## 6. Virtio Device Support

For development under QEMU/KVM, Helios includes drivers for the Virtio device family:

| Virtio Device | PCI Device ID | Purpose |
|--------------|--------------|---------|
| virtio-gpu | 0x1050 | GPU rendering (primary dev target) |
| virtio-net | 0x1041 | Network interface |
| virtio-blk | 0x1042 | Block storage (fallback if NVMe unavailable) |
| virtio-input | 0x1052 | Keyboard/mouse input |
| virtio-console | 0x1043 | Serial console |

### 6.1 Virtqueue Implementation

All Virtio devices use a common transport: **virtqueues** (split or packed).

```c
typedef struct virtqueue {
    // Descriptor table
    struct vring_desc {
        uint64_t    addr;       // Physical address of buffer
        uint32_t    len;        // Buffer length
        uint16_t    flags;      // VRING_DESC_F_NEXT, _WRITE, _INDIRECT
        uint16_t    next;       // Next descriptor index (if chained)
    } *desc;

    // Available ring (driver → device)
    struct vring_avail {
        uint16_t    flags;
        uint16_t    idx;
        uint16_t    ring[];     // Descriptor indices
    } *avail;

    // Used ring (device → driver)
    struct vring_used {
        uint16_t    flags;
        uint16_t    idx;
        struct {
            uint32_t    id;     // Descriptor chain head index
            uint32_t    len;    // Total bytes written by device
        } ring[];
    } *used;

    uint16_t        size;       // Queue size (power of 2)
    uint16_t        free_head;  // Head of free descriptor chain
    uint16_t        last_used;  // Last seen used index

    volatile uint16_t *notify;  // Doorbell register

    spinlock_t      lock;
} virtqueue_t;
```

---

## 7. Device Tree

After enumeration, the kernel maintains a global device tree accessible to all micro-programs:

```c
// Query discovered devices
int sys_device_enumerate(pcie_device_summary_t *out, uint32_t *count);

// Get detailed device info
int sys_device_info(pcie_bdf_t bdf, pcie_device_info_t *out);

// Register for device hotplug notifications
int sys_device_watch(uint64_t ipc_port);
```

---

*Next: [08-IPC.md](./08-IPC.md) — Zero-Copy Capability-Mediated IPC*
