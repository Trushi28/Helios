# 02 — Single Address Space & Capability-Based Memory

> **Subsystem:** Memory Management  
> **Owner:** Kernel team  
> **Dependencies:** x86-64 paging (PML4/PML5), UEFI memory map, PCID/INVPCID  
> **Related:** [01-BOOT.md](./01-BOOT.md), [09-SECURITY.md](./09-SECURITY.md), [08-IPC.md](./08-IPC.md)

---

## 1. The SASOS Principle

Every traditional OS creates a private virtual address space per process. This is wasteful:

- **TLB flush tax:** Every context switch invalidates cached address translations
- **Copy overhead:** IPC requires `copy_to_user`/`copy_from_user` across address spaces
- **Redundant mappings:** The kernel is mapped identically into every address space

Helios eliminates this by placing **everything** — kernel, drivers, user micro-programs, shared data — in a single, global 64-bit virtual address space. Protection is enforced not by page-table isolation, but by **capability tokens** that bound what each micro-program can access.

---

## 2. Physical Memory Manager (PMM)

### 2.1 Buddy Allocator

The PMM manages physical page frames using a **buddy allocation** scheme with the following order levels:

| Order | Block Size | Pages |
|-------|-----------|-------|
| 0 | 4 KiB | 1 |
| 1 | 8 KiB | 2 |
| 2 | 16 KiB | 4 |
| 3 | 32 KiB | 8 |
| 4 | 64 KiB | 16 |
| 5 | 128 KiB | 32 |
| 6 | 256 KiB | 64 |
| 7 | 512 KiB | 128 |
| 8 | 1 MiB | 256 |
| 9 | 2 MiB | 512 |
| 10 | 4 MiB | 1024 |

```c
#define PMM_MAX_ORDER 10

typedef struct pmm_zone {
    spinlock_t          lock;
    uint64_t            base_phys;          // Zone base physical address
    uint64_t            page_count;         // Total pages in this zone
    struct free_list    free_lists[PMM_MAX_ORDER + 1];
    uint64_t            free_page_count;    // Total free pages
    uint8_t            *bitmap;             // Allocation bitmap
} pmm_zone_t;
```

### 2.2 NUMA-Aware Zones

On multi-socket systems, the PMM creates separate zones per NUMA node (discovered via ACPI SRAT). The allocator prefers the local NUMA node for the requesting CPU core:

```c
// Per-NUMA-node zone selection
pmm_zone_t *pmm_get_local_zone(uint32_t cpu_id);

// Allocation with NUMA affinity
phys_addr_t pmm_alloc_pages(uint32_t order, uint32_t numa_node);
void        pmm_free_pages(phys_addr_t addr, uint32_t order);
```

### 2.3 Reserved Physical Regions

During PMM initialization, the following physical regions are marked as reserved (never allocated):

| Region | Purpose |
|--------|---------|
| 0x0000_0000 — 0x000F_FFFF | Legacy low memory (ISA hole, VGA buffer, BIOS data) |
| Kernel image region | The loaded kernel binary (from boot_info) |
| boot_info_t region | Boot handoff structure |
| UEFI Runtime regions | Runtime services code/data (identity-mapped) |
| ACPI NVS regions | ACPI non-volatile storage |
| MMIO regions | Memory-mapped device registers |
| NPU enclave region | Pre-allocated NPU memory (see [05-INTELLIGENCE.md](./05-INTELLIGENCE.md)) |

---

## 3. Virtual Address Space Layout

The single 48-bit canonical address space (PML4, 256 TiB) is partitioned into fixed regions:

```
64-bit Virtual Address Space (48-bit canonical, PML4)
══════════════════════════════════════════════════════════════════

0xFFFF_FFFF_FFFF_FFFF  ┌─────────────────────────────────────┐
                       │  Kernel Emergency Stack (per-core)   │  64 KiB × N cores
0xFFFF_FFFF_FFF0_0000  ├─────────────────────────────────────┤
                       │  Kernel Code & Read-Only Data        │  16 MiB
0xFFFF_FFFF_FF00_0000  ├─────────────────────────────────────┤
                       │  Kernel Heap (slab allocator)        │  1 GiB
0xFFFF_FFFF_C000_0000  ├─────────────────────────────────────┤
                       │  Per-Core Data (scheduler, TSS, etc) │  16 MiB × 256 cores
0xFFFF_FFFF_0000_0000  ├─────────────────────────────────────┤
                       │  Device MMIO Mappings                │  4 GiB
0xFFFF_FFFE_0000_0000  ├─────────────────────────────────────┤
                       │  UEFI Runtime Identity Map           │  variable
0xFFFF_FFF0_0000_0000  ├─────────────────────────────────────┤
                       │  Physical Memory Direct Map          │  up to 64 TiB
0xFFFF_FF80_0000_0000  ├─────────────────────────────────────┤
                       │                                     │
                       │  ┌─ NPU Inference Enclave ──────┐   │  configurable, up to 16 GiB
                       │  │  Model weights + KV cache    │   │
                       │  └──────────────────────────────┘   │
                       │                                     │
0xFFFF_FF00_0000_0000  ├─────────────────────────────────────┤
                       │  Object Graph Cache (hot pages)     │  up to 64 GiB
0xFFFF_FE00_0000_0000  ├─────────────────────────────────────┤
                       │                                     │
                       │  ═══ THE GAP (non-canonical) ═══    │
                       │                                     │
0x0000_8000_0000_0000  ├─────────────────────────────────────┤ ← canonical boundary
                       │                                     │
                       │  USER MICRO-PROGRAM SPACE           │
                       │  (capability-bounded regions)        │
                       │                                     │
                       │  ┌── App A region ──────────────┐   │
                       │  │  Code + Data + Stack          │   │
                       │  │  Bounded by cap token          │   │
                       │  └───────────────────────────────┘   │
                       │                                     │
                       │  ┌── App B region ──────────────┐   │
                       │  │  Code + Data + Stack          │   │
                       │  │  Bounded by cap token          │   │
                       │  └───────────────────────────────┘   │
                       │                                     │
                       │  ┌── Shared IPC region ─────────┐   │
                       │  │  Zero-copy message buffers     │   │
                       │  │  Multi-cap: A(RW) + B(RO)      │   │
                       │  └───────────────────────────────┘   │
                       │                                     │
                       │  ┌── GPU Vertex Buffer ─────────┐   │
                       │  │  Compositor data matrix        │   │
                       │  │  Cap: compositor(RW), apps(RO)  │   │
                       │  └───────────────────────────────┘   │
                       │                                     │
0x0000_0000_0100_0000  ├─────────────────────────────────────┤
                       │  Guard region (unmapped, trap null)  │  16 MiB
0x0000_0000_0000_0000  └─────────────────────────────────────┘
```

### 3.1 PML5 (5-Level Paging) Extension

If the CPU supports 5-level paging (`CPUID.07H:ECX[bit 16]`), Helios enables LA57 mode for a 57-bit virtual address space (128 PiB). This dramatically expands the user micro-program region, but the kernel region layout remains identical in the upper canonical half.

Detection and enable sequence:

```c
bool paging_supports_la57(void) {
    uint32_t ecx;
    __cpuid_count(7, 0, NULL, NULL, &ecx, NULL);
    return (ecx >> 16) & 1;
}

void paging_enable_la57(void) {
    // Must be done before enabling paging or via CR4 toggle with identity mapping
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_LA57;
    write_cr4(cr4);
    // Reload CR3 with PML5 root
}
```

---

## 4. Page Table Management

### 4.1 Shared PML4

Because all execution contexts share the same address space, there is exactly **one** PML4 (or PML5) root page table. This means:

- **No CR3 reloads on context switch** — the page table root never changes
- **No TLB flushes on context switch** — the TLB remains warm across all micro-programs
- **PCID is used for selective invalidation** — when page mappings change for a specific micro-program, `INVPCID` targets only those entries

```c
// Global page table root — never changes after SASOS init
static pml4_entry_t *g_pml4 __attribute__((aligned(4096)));

// Context switch is purely a register swap + capability load
void context_switch(microprogram_t *from, microprogram_t *to) {
    // Save/restore registers (RSP, RBP, callee-saved)
    save_context(&from->ctx);
    load_context(&to->ctx);

    // Load capability bounds for new micro-program
    cap_activate(to->cap_set);

    // NOTE: No CR3 write. No TLB flush. Nothing.
}
```

### 4.2 Page Flags

| Flag | Usage |
|------|-------|
| Present (P) | Standard presence bit |
| Read/Write (R/W) | Set for writable pages; read-only for code |
| User/Supervisor (U/S) | Set for user micro-program pages |
| No-Execute (NX) | Set for data pages; clear for executable code pages |
| Page Size (PS) | 2 MiB huge pages for kernel direct map and large allocations |
| Global (G) | Set for kernel pages (never flushed from TLB) |
| PCID (CR3 bits) | Process Context Identifier for selective TLB invalidation |

### 4.3 Huge Page Policy

| Region | Page Size | Rationale |
|--------|-----------|-----------|
| Physical memory direct map | 2 MiB / 1 GiB | Covers large physical memory with minimal page table overhead |
| Kernel code | 4 KiB | Fine-grained NX boundary control |
| NPU enclave | 2 MiB | Contiguous, large, performance-critical |
| User micro-program code | 4 KiB | Fine-grained capability boundaries |
| User micro-program data | 2 MiB | Where allocations are large enough |
| GPU vertex buffers | 2 MiB | DMA-friendly, contiguous |

---

## 5. Capability Token System

### 5.1 Token Structure

A capability token is a 256-bit unforgeable descriptor that grants access to a specific memory region with specific permissions:

```c
typedef struct {
    uint64_t base;          // Virtual base address of the region
    uint64_t length;        // Region size in bytes
    uint32_t permissions;   // Bitmask: READ | WRITE | EXEC | SHARE | DERIVE
    uint32_t owner_id;      // Micro-program ID that owns this capability
    uint64_t nonce;         // Unique random nonce (anti-replay)
    uint8_t  hmac[16];      // Truncated HMAC-SHA256 over {base, length, perms, owner, nonce}
} __attribute__((packed)) cap_token_t;
// Total: 40 bytes (padded to 48 for alignment)

_Static_assert(sizeof(cap_token_t) == 40, "cap_token_t must be 40 bytes");
```

### 5.2 Permission Bits

```c
#define CAP_PERM_READ    (1 << 0)   // Can read memory in this region
#define CAP_PERM_WRITE   (1 << 1)   // Can write memory in this region
#define CAP_PERM_EXEC    (1 << 2)   // Can execute code in this region
#define CAP_PERM_SHARE   (1 << 3)   // Can hand this cap to another micro-program
#define CAP_PERM_DERIVE  (1 << 4)   // Can create sub-capabilities (narrower bounds)
#define CAP_PERM_DMA     (1 << 5)   // Region is DMA-accessible (for drivers)
#define CAP_PERM_NOCACHE (1 << 6)   // Region is mapped uncacheable (MMIO)
#define CAP_PERM_PIN     (1 << 7)   // Pages in this region are pinned (non-swappable)
```

### 5.3 Token Lifecycle

```
┌─────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  Micro-program   │     │  Capability       │     │  Kernel           │
│  requests memory │────▶│  Manager          │────▶│  PMM allocates    │
│  (sys_cap_alloc) │     │  creates token    │     │  physical pages   │
└─────────────────┘     │  signs HMAC       │     │  maps into SASOS  │
                        │  returns to caller │     └──────────────────┘
                        └──────────────────┘

                        ┌──────────────────┐
                        │  On every memory  │
                        │  access:          │
                        │  1. Check bounds  │──── #GP if out of bounds
                        │  2. Check perms   │──── #GP if wrong permission
                        │  3. Verify HMAC   │──── #GP if tampered
                        └──────────────────┘
```

### 5.4 Hardware-Assisted Bounds Checking

On CPUs with **Intel MPX** (deprecated) or **AMD UAI**, we can use hardware bounds registers. On modern hardware without these, we implement bounds checking via:

1. **Software instrumentation at syscall boundaries** — every syscall validates capability tokens
2. **Page-table permission bits** — NX, R/W, U/S flags provide coarse-grained hardware enforcement
3. **Guard pages** — unmapped pages at capability region boundaries trap overflows
4. **Future: ARM MTE / Intel LAM** — when targeting ARM64, Memory Tagging Extension provides hardware tag-based bounds

```c
// Validate a capability token — called on every syscall that touches memory
cap_result_t cap_validate(const cap_token_t *token, uint64_t access_addr,
                          uint64_t access_size, uint32_t required_perms);

// Result type
typedef enum {
    CAP_OK              = 0,
    CAP_ERR_BOUNDS      = 1,    // Access outside [base, base+length)
    CAP_ERR_PERM        = 2,    // Missing required permission bits
    CAP_ERR_HMAC        = 3,    // HMAC verification failed (tampered token)
    CAP_ERR_REVOKED     = 4,    // Token has been revoked
    CAP_ERR_EXPIRED     = 5,    // Token TTL expired (if time-bounded)
} cap_result_t;
```

### 5.5 Capability Derivation (Sub-Capabilities)

A micro-program holding a capability with `CAP_PERM_DERIVE` can create a **sub-capability** that is strictly narrower:

```c
// Derive a sub-capability: new bounds must be within parent bounds,
// new permissions must be a subset of parent permissions.
cap_token_t cap_derive(const cap_token_t *parent,
                       uint64_t new_base, uint64_t new_length,
                       uint32_t new_perms);
```

This is the foundation of **zero-copy IPC**: Process A derives a read-only sub-capability from its data buffer and hands it to Process B. Process B can read the data directly — no copies, no kernel mediation.

### 5.6 Revocation

The kernel maintains a global **capability revocation table** (a hash map of active nonces). Revoking a capability invalidates its nonce, causing all future HMAC checks to fail:

```c
void cap_revoke(const cap_token_t *token);          // Revoke a single token
void cap_revoke_all(uint32_t owner_id);             // Revoke all tokens for a micro-program
void cap_revoke_subtree(const cap_token_t *root);   // Revoke a token and all derivatives
```

---

## 6. Slab Allocator

On top of the buddy-based PMM, the kernel uses a **slab allocator** for fixed-size kernel objects:

### 6.1 Slab Caches

```c
typedef struct slab_cache {
    const char     *name;           // Human-readable name ("cap_token", "microprogram", etc.)
    size_t          obj_size;       // Size of each object
    size_t          alignment;      // Alignment requirement
    uint32_t        objects_per_slab;
    spinlock_t      lock;
    struct slab    *partial_list;   // Slabs with free objects
    struct slab    *full_list;      // Completely allocated slabs
    struct slab    *empty_list;     // Completely free slabs (cached for reuse)
    uint64_t        total_allocs;   // Statistics
    uint64_t        total_frees;
} slab_cache_t;
```

### 6.2 Pre-Defined Caches

| Cache Name | Object Size | Purpose |
|-----------|------------|---------|
| `cap_token` | 48 bytes | Capability tokens |
| `microprogram` | 512 bytes | Micro-program control blocks |
| `sched_node` | 64 bytes | Scheduler queue nodes |
| `graph_vertex` | 128 bytes | Object graph vertices (in-memory cache) |
| `graph_edge` | 64 bytes | Object graph edges (in-memory cache) |
| `page_desc` | 32 bytes | Physical page descriptors |
| `nvme_cmd` | 64 bytes | NVMe command structures |
| `infer_req` | 256 bytes | sys_infer request descriptors |

---

## 7. Memory Allocation Syscalls

User micro-programs request memory through capability-issuing syscalls:

```c
// Allocate a new memory region and return a capability token for it
// Returns: cap_token_t on success, or error
cap_token_t sys_cap_alloc(
    uint64_t size,              // Requested size (rounded up to page boundary)
    uint32_t permissions,       // CAP_PERM_READ | CAP_PERM_WRITE | ...
    uint32_t flags              // ALLOC_HUGE_PAGES, ALLOC_CONTIGUOUS, ALLOC_ZERO, etc.
);

// Free a memory region (invalidates the capability and all derivatives)
void sys_cap_free(cap_token_t *token);

// Resize a region (may relocate). Returns new token, old token is revoked.
cap_token_t sys_cap_resize(cap_token_t *token, uint64_t new_size);

// Share a region with another micro-program (derives a sub-cap)
cap_token_t sys_cap_share(
    const cap_token_t *source,
    uint32_t target_microprog_id,
    uint32_t shared_perms        // Must be subset of source permissions
);
```

### 7.1 Allocation Flags

```c
#define ALLOC_ZERO           (1 << 0)   // Zero-fill allocated pages
#define ALLOC_HUGE_PAGES     (1 << 1)   // Use 2 MiB pages if possible
#define ALLOC_CONTIGUOUS     (1 << 2)   // Physically contiguous (for DMA)
#define ALLOC_NUMA_LOCAL     (1 << 3)   // Prefer local NUMA node
#define ALLOC_PINNED         (1 << 4)   // Pages are non-swappable
#define ALLOC_WRITE_COMBINE  (1 << 5)   // Write-combining cache policy (GPU buffers)
```

---

## 8. Demand Paging & Copy-on-Write

Even in a SASOS, not all pages need to be physically present. The VMM supports:

### 8.1 Demand Paging

Pages in a capability region can be marked as **not present** in the page table. On first access, the `#PF` handler:

1. Validates the faulting address against the micro-program's capability set
2. Allocates a physical page from the PMM
3. Maps it into the SASOS page table
4. Returns to the faulting instruction (transparent to the micro-program)

### 8.2 Copy-on-Write (CoW)

When a micro-program forks (creates a child with a copy of its capability set), the kernel does not physically copy memory. Instead:

1. All writable pages are marked read-only in the page table
2. Both parent and child receive capabilities pointing to the same physical pages
3. On first write, the `#PF` handler copies the page and updates the writing context's mapping
4. The copy is transparent — the capability token doesn't change

### 8.3 Page Fault Handler

```c
void page_fault_handler(interrupt_frame_t *frame) {
    uint64_t fault_addr = read_cr2();
    uint64_t error_code = frame->error_code;

    // 1. Is this address covered by any active capability?
    cap_token_t *cap = cap_lookup_address(current_microprog(), fault_addr);
    if (!cap) {
        // Segfault equivalent — kill micro-program
        microprog_kill(current_microprog(), KILL_REASON_CAP_VIOLATION);
        return;
    }

    // 2. Is this a demand-page fault? (page not present)
    if (!(error_code & PF_PRESENT)) {
        phys_addr_t page = pmm_alloc_pages(0, NUMA_LOCAL);
        vmm_map_page(fault_addr & PAGE_MASK, page, cap->permissions);
        return;
    }

    // 3. Is this a CoW fault? (write to read-only CoW page)
    if ((error_code & PF_WRITE) && vmm_is_cow(fault_addr)) {
        vmm_cow_resolve(fault_addr);
        return;
    }

    // 4. Genuine protection violation
    microprog_kill(current_microprog(), KILL_REASON_CAP_VIOLATION);
}
```

---

## 9. Memory-Mapped I/O (MMIO) Integration

Device MMIO regions are mapped into the SASOS with uncacheable page attributes:

```c
// Map a device's MMIO BAR into the SASOS MMIO region
// Returns a capability token for the driver micro-program
cap_token_t mmio_map_bar(pcie_device_t *dev, uint8_t bar_index,
                         uint32_t driver_microprog_id) {
    uint64_t phys_base = pcie_read_bar(dev, bar_index);
    uint64_t size      = pcie_bar_size(dev, bar_index);

    uint64_t virt = mmio_region_allocate(size);
    vmm_map_range(virt, phys_base, size,
                  PAGE_PRESENT | PAGE_WRITE | PAGE_NOCACHE | PAGE_GLOBAL);

    return cap_create(virt, size,
                      CAP_PERM_READ | CAP_PERM_WRITE | CAP_PERM_NOCACHE,
                      driver_microprog_id);
}
```

This ensures that only the authorized driver micro-program can access a device's registers, even within the shared address space.

---

## 10. DMA Safety with IOMMU

Because all micro-programs share the address space, a misbehaving DMA device could corrupt any memory. Helios uses the **IOMMU** (Intel VT-d / AMD-Vi) to constrain DMA:

```c
// Set up IOMMU domain for a PCIe device
// Only physical pages covered by the driver's DMA capability are DMA-accessible
void iommu_setup_device(pcie_device_t *dev, cap_token_t *dma_cap) {
    iommu_domain_t *domain = iommu_create_domain();

    // Map only the physical pages backing the DMA-capable region
    for (uint64_t offset = 0; offset < dma_cap->length; offset += PAGE_SIZE) {
        phys_addr_t phys = vmm_virt_to_phys(dma_cap->base + offset);
        iommu_map_page(domain, phys, phys, IOMMU_READ | IOMMU_WRITE);
    }

    iommu_attach_device(domain, dev->segment, dev->bus, dev->device, dev->function);
}
```

---

## 11. Performance Characteristics

| Operation | Traditional OS | Helios SASOS |
|-----------|---------------|-------------|
| Context switch (TLB) | Full TLB flush or ASID swap | **Zero** — same page tables |
| IPC data transfer (1 MiB) | ~50 µs (copy) | **~0 µs** (pointer handoff) |
| `mmap` new region | Create VMA, insert into rb-tree, modify per-process page tables | Allocate pages, map into global table, issue cap token |
| Syscall (memory access validation) | Check VMA tree (`O(log n)`) | Validate cap token HMAC (`O(1)`) |
| Fork | Duplicate page tables, CoW all pages | Copy cap set, CoW physical pages (no page table duplication) |

---

*Next: [03-SCHEDULER.md](./03-SCHEDULER.md) — SMP-Aware Micro-Program Scheduler*
