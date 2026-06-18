# 01 — UEFI Boot Sequence & Kernel Handoff

> **Subsystem:** Boot  
> **Owner:** Kernel team  
> **Dependencies:** UEFI 2.7+, ACPI 6.x, GOP framebuffer  
> **Related:** [02-MEMORY.md](./02-MEMORY.md), [13-ACPI-POWER.md](./13-ACPI-POWER.md), [14-INTERRUPTS.md](./14-INTERRUPTS.md)

---

## 1. Design Philosophy

Helios boots exclusively via UEFI. There is no BIOS/CSM fallback. This lets us:

- Skip 16-bit real mode and the A20 gate circus entirely
- Get a linear framebuffer from GOP before kernel entry
- Receive a structured memory map (no BIOS `int 0x15` probing)
- Load the kernel as a PE32+/COFF UEFI application directly
- Access ACPI tables via the EFI System Table pointer

---

## 2. Boot Stages

```
┌─────────────────────────────────────────────────────┐
│  UEFI Firmware                                      │
│  ├─ SEC → PEI → DXE → BDS                          │
│  └─ Loads \EFI\HELIOS\BOOTX64.EFI from ESP          │
└────────────────────┬────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────┐
│  Stage 1: UEFI Bootloader (bootx64.efi)             │
│  ├─ Locate & load kernel image from ESP              │
│  ├─ Query GOP for framebuffer base + resolution      │
│  ├─ Retrieve UEFI memory map                         │
│  ├─ Locate ACPI RSDP from EFI configuration table    │
│  ├─ Locate SMBIOS entry point                        │
│  ├─ ExitBootServices()                               │
│  └─ Jump to kernel entry with boot_info_t*           │
└────────────────────┬────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────┐
│  Stage 2: Kernel Early Init (kernel_entry)           │
│  ├─ Install GDT (64-bit long mode segments)          │
│  ├─ Install IDT (exception handlers + NMI)           │
│  ├─ Initialize serial UART for debug output           │
│  ├─ Parse UEFI memory map → build PMM free list       │
│  ├─ Set up PML4 page tables for SASOS layout          │
│  ├─ Enable CR4.PCIDE for PCID support                 │
│  ├─ Parse ACPI MADT → enumerate APIC IDs              │
│  ├─ Initialize BSP x2APIC (MSR-based)                 │
│  └─ Jump to kernel_main()                             │
└────────────────────┬────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────┐
│  Stage 3: Kernel Main                                │
│  ├─ Initialize slab allocator on top of PMM           │
│  ├─ Initialize capability manager                     │
│  ├─ Bring up AP cores via x2APIC SIPI protocol        │
│  ├─ Initialize per-core scheduler                     │
│  ├─ Enumerate PCIe topology (MCFG-based ECAM)         │
│  ├─ Initialize NVMe controller                        │
│  ├─ Mount object graph store                          │
│  ├─ Initialize GPU compositor                         │
│  ├─ Initialize NPU enclave (if present)               │
│  ├─ Launch init micro-program                         │
│  └─ Enter scheduler idle loop                         │
└─────────────────────────────────────────────────────┘
```

---

## 3. UEFI Bootloader Design

### 3.1 Source Location

```
src/boot/
├── bootx64.c          # UEFI application entry point (EFI_STATUS efi_main)
├── gop.c              # Graphics Output Protocol framebuffer acquisition
├── memory_map.c       # UEFI memory map retrieval and packaging
├── acpi.c             # RSDP/XSDT location from EFI configuration table
├── file_io.c          # Load kernel binary from ESP via Simple File System Protocol
├── boot_info.h        # boot_info_t structure definition (shared with kernel)
└── Makefile           # Build as PE32+ UEFI application
```

### 3.2 Boot Info Structure

The bootloader constructs a `boot_info_t` structure and passes its physical address to the kernel entry point. This is the **only** data channel between UEFI and the kernel.

```c
typedef struct {
    // --- Magic & Version ---
    uint64_t magic;              // 0x48454C494F53424F ("HELIOSBO")
    uint32_t version;            // Boot protocol version (1)
    uint32_t _reserved0;

    // --- Framebuffer ---
    struct {
        uint64_t base_phys;      // Physical address of linear framebuffer
        uint32_t width;          // Horizontal resolution in pixels
        uint32_t height;         // Vertical resolution in pixels
        uint32_t pitch;          // Bytes per scanline
        uint32_t bpp;            // Bits per pixel (expect 32)
        uint32_t red_mask_size;
        uint32_t red_mask_shift;
        uint32_t green_mask_size;
        uint32_t green_mask_shift;
        uint32_t blue_mask_size;
        uint32_t blue_mask_shift;
        uint32_t _pad;
    } framebuffer;

    // --- Memory Map ---
    struct {
        uint64_t entries_phys;   // Physical address of memory map entry array
        uint64_t entry_count;    // Number of entries
        uint64_t entry_size;     // Size of each entry in bytes
    } memory_map;

    // --- ACPI ---
    struct {
        uint64_t rsdp_phys;      // Physical address of ACPI RSDP (v2.0+)
    } acpi;

    // --- SMBIOS ---
    struct {
        uint64_t smbios3_phys;   // Physical address of SMBIOS 3.0 entry point
    } smbios;

    // --- Kernel Image ---
    struct {
        uint64_t phys_base;      // Where kernel was loaded in physical memory
        uint64_t size;           // Size of kernel image in bytes
        uint64_t entry_offset;   // Offset to kernel_entry from phys_base
    } kernel;

    // --- Boot Timestamps ---
    uint64_t tsc_at_exit_boot;   // TSC value at ExitBootServices() for timing
} __attribute__((packed)) boot_info_t;
```

### 3.3 Memory Map Entry Translation

We translate UEFI memory types into Helios-native types:

| UEFI Type | Helios Classification |
|-----------|----------------------|
| `EfiConventionalMemory` | `HELIOS_MEM_FREE` |
| `EfiLoaderCode`, `EfiLoaderData` | `HELIOS_MEM_BOOTLOADER` (reclaimable after init) |
| `EfiBootServicesCode`, `EfiBootServicesData` | `HELIOS_MEM_BOOTLOADER` (reclaimable) |
| `EfiRuntimeServicesCode`, `EfiRuntimeServicesData` | `HELIOS_MEM_UEFI_RUNTIME` (preserve, identity-mapped) |
| `EfiACPIReclaimMemory` | `HELIOS_MEM_ACPI_RECLAIM` |
| `EfiACPIMemoryNVS` | `HELIOS_MEM_ACPI_NVS` (preserve) |
| `EfiMemoryMappedIO`, `EfiMemoryMappedIOPortSpace` | `HELIOS_MEM_MMIO` |
| `EfiReservedMemoryType` | `HELIOS_MEM_RESERVED` |
| `EfiUnusableMemory` | `HELIOS_MEM_UNUSABLE` |

```c
typedef struct {
    uint64_t phys_base;          // Physical base address (page-aligned)
    uint64_t page_count;         // Number of 4 KiB pages
    uint32_t type;               // helios_mem_type_t enum
    uint32_t attributes;         // Cacheability, WB/WC/UC etc.
} __attribute__((packed)) helios_mem_entry_t;
```

---

## 4. Kernel Entry Contract

After `ExitBootServices()`, the bootloader transfers control with the following CPU state:

| Register / State | Value |
|-----------------|-------|
| `RDI` | Physical address of `boot_info_t` |
| `RSI` | 0 (reserved for future use) |
| CPU Mode | 64-bit long mode (already set by UEFI) |
| Paging | UEFI identity mapping still active |
| Interrupts | Disabled (`CLI`) |
| `CR0` | PG=1, PE=1, WP=1 |
| `CR4` | PAE=1 (others as UEFI left them) |
| `EFER` | LME=1, LMA=1, NXE=1 |
| Stack | 64 KiB stack allocated by bootloader in conventional memory |

### 4.1 Why we don't use Limine/BOOTBOOT/Multiboot

These protocols impose assumptions about memory layout, higher-half kernel mapping conventions, and page table structures that conflict with our SASOS design. Our single-address-space layout requires full control over the PML4 from the earliest moment. A custom bootloader gives us:

- Direct control over the initial page table topology
- Ability to pre-allocate contiguous physical regions for the NPU memory enclave
- Guaranteed identity mapping preservation for UEFI runtime services
- Boot-time GPU framebuffer acquisition before kernel entry

---

## 5. GDT Configuration

The kernel installs a minimal 64-bit GDT immediately upon entry:

```
Index  Selector  Description                  Base   Limit    Access    Flags
─────  ────────  ───────────────────────────  ─────  ───────  ────────  ─────
0      0x00      Null descriptor              0      0        0x00      0x0
1      0x08      Kernel Code (64-bit, Ring 0) 0      0xFFFFF  0x9A      0xA
2      0x10      Kernel Data (64-bit, Ring 0) 0      0xFFFFF  0x92      0xC
3      0x18      User Code (64-bit, Ring 3)   0      0xFFFFF  0xFA      0xA
4      0x20      User Data (64-bit, Ring 3)   0      0xFFFFF  0xF2      0xC
5      0x28      TSS (per-core, 16 bytes)     varies 0x67     0x89      0x0
```

> **Note on SASOS and Ring 3:** Even though we share a single address space, we still use Ring 0/Ring 3 separation. User micro-programs run in Ring 3 and trigger capability faults (via `#GP` or `#PF`) if they access memory outside their token bounds. The kernel runs in Ring 0 to manage capabilities, handle interrupts, and program hardware.

---

## 6. Early Serial Debug Output

Before any framebuffer rendering is initialized, the kernel writes diagnostic output to **COM1 (0x3F8)**. This is critical for debugging bare-metal boot on real hardware.

```c
// Minimal polled UART output — no interrupts, no FIFOs initially
void serial_init(void);                    // 115200 baud, 8N1
void serial_putc(char c);                  // Blocking polled write
void serial_puts(const char *s);           // String output
void serial_printf(const char *fmt, ...);  // Minimal printf subset
```

The serial interface remains active throughout the kernel's lifetime as a debug backchannel. In later phases, a debug micro-program provides structured logging over serial.

---

## 7. ACPI Table Discovery

The bootloader provides the RSDP physical address. The kernel parses the ACPI table hierarchy during early init:

```
RSDP → XSDT → { MADT, MCFG, FADT, HPET, DMAR, ... }
```

### Critical Tables for Boot

| Table | Purpose |
|-------|---------|
| **MADT** | Enumerate Local APIC IDs for SMP, I/O APIC base addresses, NMI sources |
| **MCFG** | PCIe ECAM base address for MMIO-based configuration space access |
| **FADT** | Power management profile, century register, boot flags |
| **HPET** | High Precision Event Timer base address (fallback if TSC is unreliable) |
| **DMAR** | Intel VT-d / IOMMU DMA Remapping engine locations |
| **SRAT** | System Resource Affinity Table — NUMA topology |
| **SLIT** | System Locality Information — NUMA latency matrix |
| **BGRT** | Boot Graphics Resource — OEM splash screen (we ignore this) |

### MADT Parsing for SMP

The MADT contains a variable-length array of interrupt controller structures. We scan for:

- **Type 0x00 — Processor Local APIC:** Legacy, but we read for APIC ID enumeration
- **Type 0x09 — Processor Local x2APIC:** Preferred. Contains 32-bit x2APIC ID
- **Type 0x01 — I/O APIC:** I/O APIC base address and GSI base
- **Type 0x02 — Interrupt Source Override:** ISA IRQ → GSI remapping
- **Type 0x04 — Local APIC NMI:** NMI LINT# configuration per processor

```c
typedef struct {
    uint32_t x2apic_id;      // 32-bit x2APIC ID
    uint32_t acpi_uid;       // ACPI processor UID
    uint32_t flags;          // bit 0: enabled, bit 1: online-capable
    uint8_t  bsp;            // 1 if this is the bootstrap processor
} helios_cpu_info_t;

// Populated during MADT parse. Max 256 cores for initial implementation.
#define HELIOS_MAX_CPUS 256
extern helios_cpu_info_t g_cpu_table[HELIOS_MAX_CPUS];
extern uint32_t          g_cpu_count;
```

---

## 8. Transition to Higher-Level Init

Once early init completes (GDT, IDT, serial, PMM, paging, ACPI parse), the kernel calls `kernel_main()` which orchestrates the rest of the boot sequence in C:

```c
_Noreturn void kernel_main(boot_info_t *boot_info) {
    // Phase 1: Core memory subsystems
    pmm_init(boot_info->memory_map);
    vmm_init_sasos();         // Install SASOS PML4
    slab_init();              // Slab allocator on top of PMM
    cap_manager_init();       // Capability token infrastructure

    // Phase 2: SMP and scheduling
    smp_init();               // Send INIT-SIPI-SIPI to APs via x2APIC
    scheduler_init();         // Per-core run queues

    // Phase 3: Device discovery
    pcie_enumerate();         // Walk ECAM, build device tree
    nvme_init();              // Initialize NVMe controller(s)
    objstore_mount();         // Mount object graph from NVMe

    // Phase 4: Display and input
    gpu_init();               // GPU driver + compositor
    usb_xhci_init();          // USB 3.x host controller
    hid_init();               // Keyboard + mouse via USB HID

    // Phase 5: Intelligence
    npu_init();               // Reserve NPU memory, load base model
    sys_infer_init();         // Register sys_infer syscall

    // Phase 6: User space
    init_microprog_launch();  // Load and execute the init micro-program

    // This core becomes the idle thread for CPU 0
    scheduler_idle_loop();
}
```

---

## 9. Disk Layout (ESP)

The boot disk is a GPT-partitioned image:

```
GPT Disk Layout
──────────────────────────────────────────────
 Partition 1: EFI System Partition (FAT32)
   /EFI/HELIOS/BOOTX64.EFI    — UEFI bootloader
   /EFI/HELIOS/KERNEL.BIN     — Flat binary kernel image
   /EFI/HELIOS/INITDATA.BIN   — Initial object graph seed (optional)
   /EFI/HELIOS/BASEMODEL.BIN  — Compressed NPU base model (optional)

 Partition 2: Helios Object Store (custom format)
   Raw NVMe namespace managed by the object graph engine.
   No traditional file system. See 04-STORAGE.md.
──────────────────────────────────────────────
```

---

## 10. Error Handling During Boot

Boot failures are reported via:

1. **Serial output** — Always available, primary debug channel
2. **GOP framebuffer** — If available, render a diagnostic panic screen
3. **ACPI shutdown** — On unrecoverable errors, attempt clean ACPI power-off

Panic screen format:
```
╔══════════════════════════════════════════════╗
║  HELIOS BOOT PANIC                           ║
║                                              ║
║  Stage:    Early Init                        ║
║  Error:    PMM: No usable memory regions     ║
║  RIP:      0xFFFFFFFF80001A3C                ║
║  RSP:      0xFFFFFFFF80100FF0                ║
║  CR2:      0x0000000000000000                ║
║                                              ║
║  Stack Trace:                                ║
║    [0] kernel_entry+0x1A3C                   ║
║    [1] pmm_init+0x0042                       ║
║    [2] _start+0x0010                         ║
║                                              ║
║  Serial log available at COM1 (115200 8N1)   ║
╚══════════════════════════════════════════════╝
```

---

*Next: [02-MEMORY.md](./02-MEMORY.md) — Single Address Space & Capability-Based Memory*
