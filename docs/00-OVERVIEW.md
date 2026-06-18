# Helios Operating System — Design Overview

> **Codename:** Helios  
> **Version:** 0.1.0-draft  
> **Target Architecture:** x86-64 (AMD64), with future ARM64/RISC-V ports  
> **Primary Language:** C (C23), with inline assembly for architecture-specific paths  
> **License:** TBD  
> **Status:** Design Phase

---

## 1. Vision Statement

Helios is a **from-scratch, bare-metal operating system** engineered for modern hardware. It rejects legacy compatibility in favor of a clean-slate design that exploits the full capability of contemporary silicon: multi-core processors with x2APIC, NVMe storage controllers, IOMMU virtualization, hardware NPUs, and GPU compute pipelines.

The design rests on four radical pillars:

| Pillar | Traditional OS | Helios |
|--------|---------------|--------|
| **Memory** | Per-process virtual address spaces, expensive TLB flushes | Single Address Space with capability-based protection |
| **Storage** | Hierarchical file system (ext4, NTFS, APFS) | Cryptographic content-addressed object graph (DAG) |
| **Intelligence** | User-space AI libraries, cloud APIs | Kernel-managed NPU scheduling via `sys_infer` syscall |
| **Display** | Widget toolkit compositing (GTK, Qt, Win32) | GPU-native vertex-matrix compositor with structural shaders |

These four pillars are unified by a single principle: **the operating system is a flat, globally-addressed data fabric** where memory, storage, intelligence, and rendering are projections of the same underlying object graph.

---

## 2. Non-Goals & Anti-Patterns

To maintain clarity, we explicitly reject:

- **POSIX compliance.** We do not emulate Unix semantics. No `fork()`, no `/proc`, no file descriptors as integers.
- **Legacy hardware support.** No ISA bus, no PIC/8259A, no IDE/ATA, no PS/2 (except during early keyboard init before USB HID).
- **Legacy boot.** BIOS/MBR boot is not supported. UEFI only.
- **Monolithic driver blobs.** All drivers are isolated micro-programs communicating via capability-mediated shared memory.
- **Dynamic linking at the traditional level.** Code is loaded as content-addressed objects; the linker is a graph resolver.

---

## 3. Target Hardware Baseline

Helios targets a **minimum hardware floor** of circa-2020 x86-64 platforms:

| Component | Minimum Requirement | Used Feature |
|-----------|-------------------|--------------|
| CPU | 64-bit x86 with x2APIC | SMP, x2APIC MSR interface, PCID, INVPCID |
| Memory | 4 GiB DDR4 | 4-level or 5-level paging (PML4/PML5) |
| Storage | NVMe SSD | NVMe 1.4+ submission/completion queue protocol |
| GPU | Vulkan 1.2 capable | Compute shaders, direct framebuffer for early boot |
| Firmware | UEFI 2.7+ | GOP framebuffer, memory map, ACPI 6.x tables |
| Optional | Discrete NPU or GPU tensor cores | `sys_infer` acceleration |
| Network | PCIe Ethernet (Intel i210/i225 class) | Virtio-net for VM testing |

---

## 4. Document Map

This design specification is split across the following documents:

| Document | Description |
|----------|-------------|
| [01-BOOT.md](./01-BOOT.md) | UEFI boot flow, kernel handoff, early hardware init |
| [02-MEMORY.md](./02-MEMORY.md) | SASOS layout, capability tokens, physical memory manager |
| [03-SCHEDULER.md](./03-SCHEDULER.md) | SMP-aware micro-program scheduler, x2APIC IPI protocol |
| [04-STORAGE.md](./04-STORAGE.md) | NVMe driver, object graph file system, content addressing |
| [05-INTELLIGENCE.md](./05-INTELLIGENCE.md) | NPU abstraction, `sys_infer` syscall, model memory region |
| [06-COMPOSITOR.md](./06-COMPOSITOR.md) | Vertex-matrix UI engine, structural shaders, GPU pipeline |
| [07-DRIVERS.md](./07-DRIVERS.md) | Driver isolation model, PCIe enumeration, IOMMU integration |
| [08-IPC.md](./08-IPC.md) | Zero-copy capability-mediated IPC, signal graph |
| [09-SECURITY.md](./09-SECURITY.md) | Capability system, secure boot chain, object signing |
| [10-NETWORKING.md](./10-NETWORKING.md) | TCP/IP stack, zero-copy packet buffers, socket-less API |
| [11-SHELL.md](./11-SHELL.md) | Graph-query shell, AI-assisted command pipeline |
| [12-BUILD.md](./12-BUILD.md) | Build system, cross-compilation, testing infrastructure |
| [13-ACPI-POWER.md](./13-ACPI-POWER.md) | ACPI table parsing, power management, thermal control |
| [14-INTERRUPTS.md](./14-INTERRUPTS.md) | IDT, x2APIC, MSI/MSI-X, interrupt routing |
| [15-USB.md](./15-USB.md) | xHCI USB 3.x host controller, HID input |
| [16-AUDIO.md](./16-AUDIO.md) | Intel HDA controller, low-latency audio pipeline |
| [17-ROADMAP.md](./17-ROADMAP.md) | Phased development milestones, MVP definition |

---

## 5. Architectural Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                     SINGLE 64-BIT VIRTUAL ADDRESS SPACE             │
│                                                                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐           │
│  │ App A    │  │ App B    │  │ App C    │  │ Shell    │           │
│  │ (cap-    │  │ (cap-    │  │ (cap-    │  │ (cap-    │           │
│  │  bounded)│  │  bounded)│  │  bounded)│  │  bounded)│           │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘           │
│       │              │              │              │                 │
│       ▼              ▼              ▼              ▼                 │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │              CAPABILITY ENFORCEMENT LAYER                   │    │
│  │         (hardware bounds + crypto token validation)         │    │
│  └─────────────────────────┬───────────────────────────────────┘    │
│                             │                                       │
│  ┌──────────────────────────▼──────────────────────────────────┐    │
│  │                    HELIOS MICRO-KERNEL                       │    │
│  │  ┌───────────┐ ┌────────────┐ ┌────────┐ ┌──────────────┐  │    │
│  │  │ Scheduler │ │ Cap Manager│ │ PMM    │ │ sys_infer    │  │    │
│  │  │ (per-core)│ │            │ │ (buddy)│ │ dispatcher   │  │    │
│  │  └───────────┘ └────────────┘ └────────┘ └──────────────┘  │    │
│  └──────────────────────────┬──────────────────────────────────┘    │
│                             │                                       │
│  ┌──────────────────────────▼──────────────────────────────────┐    │
│  │                 HARDWARE ABSTRACTION                         │    │
│  │  ┌───────┐ ┌───────┐ ┌───────┐ ┌──────┐ ┌──────┐ ┌──────┐ │    │
│  │  │x2APIC │ │ NVMe  │ │ GPU   │ │ NPU  │ │ xHCI │ │ NIC  │ │    │
│  │  └───────┘ └───────┘ └───────┘ └──────┘ └──────┘ └──────┘ │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │             OBJECT GRAPH STORAGE ENGINE                      │    │
│  │        (content-addressed DAG on NVMe, append-only)          │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Core Concepts Glossary

| Term | Definition |
|------|-----------|
| **Micro-program** | The Helios equivalent of a "process." A capability-bounded execution context within the single address space. |
| **Capability Token** | A cryptographic pointer: `{base_addr, length, permissions, HMAC}`. Hardware-enforced bounds. |
| **Object** | The fundamental unit of persistent storage. Immutable, content-addressed by SHA-256. |
| **Vertex** | A node in the object graph. Every object, relationship, tag, or metadata entity is a vertex. |
| **Edge** | A directed, typed link between vertices in the object graph. Replaces directory hierarchy. |
| **Structural Shader** | A user-defined mathematical function that maps the UI data matrix to GPU vertex positions. |
| **Data Matrix** | The global reactive state buffer driving the compositor. Updated by micro-programs, consumed by shaders. |
| **sys_infer** | Native system call for submitting inference requests to the kernel-managed NPU/tensor enclave. |
| **SASOS** | Single Address Space Operating System — all code shares one virtual address space. |
| **PMM** | Physical Memory Manager — buddy allocator for physical page frames. |
| **Cap Manager** | Capability Manager — issues, validates, and revokes capability tokens. |

---

## 7. Language & Toolchain

```
Compiler:       GCC 14+ or Clang 18+ (freestanding, -ffreestanding -nostdlib)
Assembler:      NASM 2.16+ (for boot stubs and architecture-specific primitives)
Linker:         GNU ld or LLD with custom linker scripts
Build System:   GNU Make + custom Python 3.12+ meta-build scripts
Debug:          GDB remote stub over serial/TCP, QEMU/KVM for virtualized testing
Hardware Test:  Physical x86-64 machine with UEFI, NVMe, discrete GPU
Image Format:   GPT disk image with EFI System Partition (FAT32)
Firmware:       UEFI application (.efi) as stage-1 bootloader
```

### C Coding Standard

- **Standard:** C23 (ISO/IEC 9899:2024) with GNU extensions where necessary
- **No dynamic allocation in kernel hot paths** — all kernel structures use slab/buddy allocation
- **All pointer arithmetic is capability-bounded** at the type level
- **Naming:** `snake_case` for functions and variables, `UPPER_SNAKE` for constants, `TypeName_t` for types
- **Documentation:** Every public function has a Doxygen-style `/** */` comment block
- **Error handling:** No errno. Functions return tagged result types: `result_t<T, err_t>`

---

## 8. Development Phases

See [17-ROADMAP.md](./17-ROADMAP.md) for the full phased plan. Summary:

| Phase | Milestone | Estimated Duration |
|-------|----------|-------------------|
| **Phase 0** | UEFI boot → kernel entry → serial output | 2–3 weeks |
| **Phase 1** | PMM + SASOS paging + capability stubs | 3–4 weeks |
| **Phase 2** | SMP bring-up + x2APIC + scheduler | 3–4 weeks |
| **Phase 3** | NVMe driver + object graph store | 4–6 weeks |
| **Phase 4** | GPU framebuffer + vertex compositor | 4–6 weeks |
| **Phase 5** | NPU/tensor integration + sys_infer | 3–4 weeks |
| **Phase 6** | Shell + user-space micro-programs | 3–4 weeks |
| **Phase 7** | Networking + USB + audio | 4–6 weeks |
| **Phase 8** | Polish, testing, bare-metal validation | Ongoing |

---

*Next: [01-BOOT.md](./01-BOOT.md) — UEFI Boot Sequence & Kernel Handoff*
