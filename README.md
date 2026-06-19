# Helios Operating System

A from-scratch, bare-metal **x86-64 operating system** designed for modern hardware.

No legacy support. No POSIX. No monolithic drivers.

## Architecture

| Component | Design |
|-----------|--------|
| **Boot** | UEFI-only (no BIOS/MBR), Secure Boot chain |
| **Memory** | Single Address Space OS (SASOS), capability-token security |
| **Scheduler** | Per-core O(1), work-stealing, CFS-inspired fairness |
| **Storage** | Content-addressed (SHA-256) immutable object graph |
| **Intelligence** | Kernel-managed AI inference (`sys_infer`) via NPU enclave |
| **Compositor** | GPU-accelerated vertex-matrix display system |
| **Drivers** | Ring 3 isolated micro-programs, PCIe/MSI-X only |
| **IPC** | Zero-copy capability-mediated shared memory |
| **Networking** | Zero-copy TCP/IP stack, no sockets (capability-based) |

## Quick Start

### Prerequisites

```bash
# Cross-compiler toolchain
# Build or install x86_64-elf-gcc, x86_64-elf-ld, x86_64-elf-objcopy

# Assembler
sudo apt install nasm

# UEFI firmware for QEMU
sudo apt install ovmf

# Disk image tools
sudo apt install dosfstools mtools gdisk

# Emulator
sudo apt install qemu-system-x86
```

### Build & Run

```bash
# Build everything and create disk image
make

# Build and run in QEMU
make run

# Build and run with GDB debug stub
make debug
```

### GDB Debugging

```bash
# Terminal 1: Run with debug
make debug

# Terminal 2: Connect GDB
gdb build/kernel/kernel.elf -ex 'target remote :1234'
```

## Project Structure

```
helios/
├── docs/                     Design documentation (18 documents)
├── src/
│   ├── boot/                 UEFI bootloader (PE32+ application)
│   ├── kernel/               Kernel core
│   │   ├── arch/x86_64/      Architecture-specific (GDT, IDT, APIC)
│   │   ├── mm/               Memory management (Phase 1)
│   │   ├── sched/            Scheduler + SMP (Phase 2)
│   │   ├── ipc/              IPC subsystem (Phase 2)
│   │   └── crypto/           SHA-256, HMAC, Ed25519 (Phase 3)
│   ├── drivers/              Driver micro-programs (user-space)
│   ├── services/             System services (object store, compositor)
│   ├── lib/                  User-space libraries
│   └── include/              Shared headers
│       ├── helios/           OS-wide types and structures
│       └── arch/x86_64/      Architecture-specific definitions
├── linker/                   Linker scripts (kernel, bootloader, µprogram)
├── tools/                    Build tools and scripts
├── tests/                    Unit, integration, and stress tests
├── Makefile                  Top-level build orchestration
└── config.mk                Compiler flags and paths
```

## Build Targets

| Target | Description |
|--------|-------------|
| `make` | Build everything (bootloader + kernel + disk image) |
| `make boot` | Build UEFI bootloader only |
| `make kernel` | Build kernel only |
| `make image` | Build disk image |
| `make run` | Build and run in QEMU |
| `make debug` | Build and run with GDB stub on port 1234 |
| `make clean` | Remove all build artifacts |

## Development Phases

| Phase | Status | Description |
|-------|--------|-------------|
| **0** | ✅ | UEFI boot → kernel → serial "Hello World" |
| **1** | ☐ | Physical memory manager, SASOS page tables, capability stubs |
| **2** | ☐ | SMP bring-up, per-core scheduler, context switching |
| **3** | ☐ | NVMe driver, content-addressed object store |
| **4** | ☐ | GPU compositor, text rendering, data matrix |
| **5** | ☐ | NPU enclave, sys_infer, AI completions |
| **6** | ☐ | Graph-query shell, user-space micro-programs |
| **7** | ☐ | TCP/IP networking, USB HID, audio |
| **8** | ☐ | Bare-metal validation, performance tuning |

## License

Proprietary — all rights reserved.
