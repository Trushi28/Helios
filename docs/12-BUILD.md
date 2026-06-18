# 12 — Build System & Cross-Compilation

> **Subsystem:** Build Infrastructure  
> **Owner:** DevOps / All teams  
> **Dependencies:** GCC/Clang, NASM, GNU Make, Python 3.12+, QEMU  
> **Related:** [01-BOOT.md](./01-BOOT.md), [00-OVERVIEW.md](./00-OVERVIEW.md)

---

## 1. Toolchain Requirements

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| GCC | 14.0+ | Kernel + micro-program C compiler (freestanding) |
| Clang/LLVM | 18.0+ | Alternative compiler, static analysis |
| NASM | 2.16+ | x86-64 assembly (boot stubs, trampoline) |
| GNU ld / LLD | 2.42+ / 18.0+ | Linking with custom linker scripts |
| GNU Make | 4.4+ | Primary build orchestration |
| Python | 3.12+ | Meta-build scripts, image generation, testing |
| QEMU | 8.2+ | Emulation target (KVM acceleration) |
| OVMF | Latest | UEFI firmware for QEMU |
| GDB | 14.0+ | Remote debugging over serial/TCP |
| xorriso | 1.5.6+ | ISO/GPT image creation |
| mtools | 4.0.43+ | FAT32 ESP image manipulation |
| dosfstools | 4.2+ | FAT32 filesystem creation |
| parted/gdisk | - | GPT partition table creation |

---

## 2. Source Tree Layout

```
helios/
├── docs/                       # Design documentation (this folder)
│   ├── 00-OVERVIEW.md
│   ├── 01-BOOT.md
│   └── ...
│
├── src/
│   ├── boot/                   # UEFI bootloader (PE32+ application)
│   │   ├── bootx64.c           # EFI_STATUS efi_main()
│   │   ├── gop.c               # GOP framebuffer acquisition
│   │   ├── memory_map.c        # UEFI memory map retrieval
│   │   ├── acpi.c              # RSDP location
│   │   ├── file_io.c           # ESP file loading
│   │   ├── boot_info.h         # Shared boot_info_t definition
│   │   └── Makefile
│   │
│   ├── kernel/                 # Kernel core
│   │   ├── entry.asm           # kernel_entry (ASM stub: GDT, IDT, call kernel_main)
│   │   ├── main.c              # kernel_main()
│   │   ├── panic.c             # Kernel panic handler
│   │   ├── serial.c            # UART debug output
│   │   ├── string.c            # memcpy, memset, strlen, etc.
│   │   ├── printf.c            # Minimal printf implementation
│   │   │
│   │   ├── mm/                 # Memory management
│   │   │   ├── pmm.c           # Physical memory manager (buddy)
│   │   │   ├── vmm.c           # Virtual memory manager (SASOS page tables)
│   │   │   ├── slab.c          # Slab allocator
│   │   │   ├── capability.c    # Capability token manager
│   │   │   └── iommu.c         # IOMMU (VT-d) driver
│   │   │
│   │   ├── sched/              # Scheduler
│   │   │   ├── scheduler.c     # Per-core scheduler
│   │   │   ├── smp.c           # SMP bring-up (INIT-SIPI-SIPI)
│   │   │   ├── context.asm     # Context switch (save/restore registers)
│   │   │   ├── idle.c          # Idle loop + work stealing
│   │   │   └── microprog.c     # Micro-program lifecycle
│   │   │
│   │   ├── arch/               # Architecture-specific
│   │   │   ├── x86_64/
│   │   │   │   ├── gdt.c       # GDT setup
│   │   │   │   ├── idt.c       # IDT + exception handlers
│   │   │   │   ├── x2apic.c    # x2APIC driver
│   │   │   │   ├── tsc.c       # TSC calibration
│   │   │   │   ├── cpuid.c     # CPUID feature detection
│   │   │   │   ├── msr.c       # MSR read/write helpers
│   │   │   │   └── paging.c    # Page table manipulation
│   │   │   └── (future: aarch64/, riscv64/)
│   │   │
│   │   ├── acpi/               # ACPI table parsing
│   │   │   ├── acpi.c          # RSDP/XSDT parser
│   │   │   ├── madt.c          # MADT (APIC enumeration)
│   │   │   ├── mcfg.c          # MCFG (PCIe ECAM)
│   │   │   ├── fadt.c          # FADT (power management)
│   │   │   └── hpet.c          # HPET timer
│   │   │
│   │   ├── ipc/                # IPC subsystem
│   │   │   ├── port.c          # IPC ports
│   │   │   ├── message.c       # Message send/recv
│   │   │   ├── signal.c        # Signal graph
│   │   │   └── service.c       # Named service registry
│   │   │
│   │   ├── infer/              # NPU / inference subsystem
│   │   │   ├── enclave.c       # NPU memory enclave
│   │   │   ├── scheduler.c     # Inference request scheduler
│   │   │   ├── tokenizer.c     # BPE tokenizer
│   │   │   ├── backend_cpu.c   # CPU SIMD inference backend
│   │   │   ├── backend_gpu.c   # GPU compute inference backend
│   │   │   └── kv_cache.c      # KV cache manager
│   │   │
│   │   ├── syscall/            # System call interface
│   │   │   ├── syscall.c       # Syscall dispatch table
│   │   │   ├── syscall_entry.asm # SYSCALL/SYSRET entry stub
│   │   │   └── syscall_table.h # Syscall number definitions
│   │   │
│   │   └── crypto/             # Cryptographic primitives
│   │       ├── sha256.c
│   │       ├── hmac.c
│   │       ├── ed25519.c
│   │       └── random.c        # RDRAND/RDSEED wrapper
│   │
│   ├── drivers/                # Driver micro-programs (user-space)
│   │   ├── nvme/               # NVMe storage driver
│   │   │   ├── nvme.c
│   │   │   ├── nvme_queue.c
│   │   │   └── Makefile
│   │   ├── gpu/                # GPU drivers
│   │   │   ├── virtio_gpu.c    # Virtio-GPU (QEMU)
│   │   │   └── Makefile
│   │   ├── net/                # Network drivers
│   │   │   ├── virtio_net.c    # Virtio-Net (QEMU)
│   │   │   ├── e1000e.c        # Intel E1000e
│   │   │   └── Makefile
│   │   ├── usb/                # USB drivers
│   │   │   ├── xhci.c          # xHCI host controller
│   │   │   ├── hid.c           # USB HID (keyboard/mouse)
│   │   │   └── Makefile
│   │   └── audio/              # Audio drivers
│   │       ├── hda.c           # Intel HDA controller
│   │       └── Makefile
│   │
│   ├── services/               # System service micro-programs
│   │   ├── objstore/           # Object graph store engine
│   │   │   ├── objstore.c
│   │   │   ├── graph.c
│   │   │   ├── transaction.c
│   │   │   ├── gc.c
│   │   │   └── Makefile
│   │   ├── compositor/         # Vertex-matrix compositor
│   │   │   ├── compositor.c
│   │   │   ├── text_render.c
│   │   │   ├── layout.c
│   │   │   ├── animation.c
│   │   │   └── Makefile
│   │   ├── netstack/           # TCP/IP network stack
│   │   │   ├── netstack.c
│   │   │   ├── tcp.c
│   │   │   ├── udp.c
│   │   │   ├── ip.c
│   │   │   ├── arp.c
│   │   │   ├── dhcp.c
│   │   │   └── Makefile
│   │   └── shell/              # Shell
│   │       ├── shell.c
│   │       ├── parser.c
│   │       ├── pipeline.c
│   │       ├── completion.c
│   │       └── Makefile
│   │
│   ├── lib/                    # Shared libraries (user-space)
│   │   ├── libc/               # Minimal C runtime for micro-programs
│   │   │   ├── string.c
│   │   │   ├── stdio.c
│   │   │   ├── stdlib.c
│   │   │   ├── math.c
│   │   │   └── crt0.asm        # _start entry point for micro-programs
│   │   ├── libhelios/          # Helios syscall wrappers
│   │   │   ├── cap.c
│   │   │   ├── ipc.c
│   │   │   ├── ui.c
│   │   │   ├── net.c
│   │   │   ├── infer.c
│   │   │   └── obj.c
│   │   └── libgraph/           # Graph query library
│   │       ├── query.c
│   │       └── traverse.c
│   │
│   └── include/                # Shared header files
│       ├── helios/
│       │   ├── types.h         # Common types (uint64_t, phys_addr_t, etc.)
│       │   ├── boot_info.h     # boot_info_t (shared boot/kernel)
│       │   ├── capability.h    # cap_token_t, permission defines
│       │   ├── syscall.h       # Syscall numbers and wrappers
│       │   ├── ipc.h           # IPC types and message format
│       │   ├── object.h        # object_id_t, vertex/edge types
│       │   ├── microprog.h     # Micro-program types
│       │   └── error.h         # Error codes
│       └── arch/
│           └── x86_64/
│               ├── msr.h
│               ├── cpuid.h
│               ├── apic.h
│               └── paging.h
│
├── tools/                      # Build tools and scripts
│   ├── mkimage.py              # Build disk image (GPT + ESP + object store)
│   ├── mkfont.py               # Convert TTF → glyph atlas binary
│   ├── sign.py                 # Sign kernel/driver binaries (Ed25519)
│   ├── qemu-run.sh             # Launch QEMU with correct flags
│   ├── gdb-connect.sh          # Connect GDB to QEMU debug stub
│   └── test_runner.py          # Automated test execution
│
├── linker/                     # Linker scripts
│   ├── kernel.ld               # Kernel linker script
│   ├── bootloader.ld           # UEFI bootloader linker script
│   └── microprog.ld            # User micro-program linker script
│
├── tests/                      # Test suite
│   ├── unit/                   # Unit tests (run on host)
│   │   ├── test_pmm.c
│   │   ├── test_slab.c
│   │   ├── test_capability.c
│   │   ├── test_sha256.c
│   │   ├── test_graph.c
│   │   └── Makefile
│   ├── integration/            # Integration tests (run in QEMU)
│   │   ├── test_boot.py
│   │   ├── test_smp.py
│   │   ├── test_nvme.py
│   │   └── test_ipc.py
│   └── stress/                 # Stress tests
│       ├── stress_alloc.c
│       ├── stress_ipc.c
│       └── stress_infer.c
│
├── Makefile                    # Top-level Makefile
├── config.mk                  # Build configuration (compiler flags, paths)
└── README.md                  # Project readme
```

---

## 3. Build Configuration

### config.mk

```makefile
# ─── Toolchain ───
CC          := x86_64-elf-gcc
AS          := nasm
LD          := x86_64-elf-ld
OBJCOPY     := x86_64-elf-objcopy
AR          := x86_64-elf-ar

# ─── Compiler Flags ───
CFLAGS_COMMON := -std=c2x -Wall -Wextra -Werror -Wpedantic \
                 -fno-exceptions -fno-unwind-tables \
                 -fstack-protector-strong \
                 -Isrc/include

CFLAGS_KERNEL := $(CFLAGS_COMMON) \
                 -ffreestanding -nostdlib -nostdinc \
                 -mcmodel=kernel -mno-red-zone \
                 -mno-mmx -mno-sse -mno-sse2 \
                 -fno-pic -fno-pie \
                 -DHELIOS_KERNEL

CFLAGS_BOOT   := $(CFLAGS_COMMON) \
                 -ffreestanding -nostdlib \
                 -fno-pic \
                 -target x86_64-unknown-windows \
                 -DHELIOS_BOOT

CFLAGS_USER   := $(CFLAGS_COMMON) \
                 -ffreestanding -nostdlib \
                 -fpic -fPIE \
                 -DHELIOS_USER

# ─── Assembler Flags ───
ASFLAGS     := -f elf64 -g -F dwarf

# ─── Linker Flags ───
LDFLAGS_KERNEL := -T linker/kernel.ld -nostdlib -static
LDFLAGS_BOOT   := -T linker/bootloader.ld -nostdlib \
                  -subsystem:efi_application -entry:efi_main
LDFLAGS_USER   := -T linker/microprog.ld -nostdlib -pie

# ─── Debug ───
DEBUG       ?= 1
ifeq ($(DEBUG),1)
    CFLAGS_COMMON += -g3 -O0 -DHELIOS_DEBUG
else
    CFLAGS_COMMON += -O2 -DNDEBUG
endif

# ─── Paths ───
BUILD_DIR   := build
ISO_DIR     := $(BUILD_DIR)/iso
ESP_DIR     := $(BUILD_DIR)/esp
IMAGE       := $(BUILD_DIR)/helios.img
```

---

## 4. Build Targets

### Top-Level Makefile

```makefile
.PHONY: all boot kernel drivers services image run debug clean

all: image

boot:
    $(MAKE) -C src/boot

kernel:
    $(MAKE) -C src/kernel

drivers: kernel
    $(MAKE) -C src/drivers/nvme
    $(MAKE) -C src/drivers/gpu
    $(MAKE) -C src/drivers/net
    $(MAKE) -C src/drivers/usb
    $(MAKE) -C src/drivers/audio

services: kernel
    $(MAKE) -C src/services/objstore
    $(MAKE) -C src/services/compositor
    $(MAKE) -C src/services/netstack
    $(MAKE) -C src/services/shell

lib:
    $(MAKE) -C src/lib/libc
    $(MAKE) -C src/lib/libhelios

image: boot kernel drivers services
    python3 tools/mkimage.py \
        --bootloader $(BUILD_DIR)/boot/BOOTX64.EFI \
        --kernel $(BUILD_DIR)/kernel/kernel.bin \
        --drivers $(BUILD_DIR)/drivers/ \
        --services $(BUILD_DIR)/services/ \
        --output $(IMAGE)

run: image
    bash tools/qemu-run.sh $(IMAGE)

debug: image
    bash tools/qemu-run.sh $(IMAGE) --debug

test-unit:
    $(MAKE) -C tests/unit run

test-integration: image
    python3 tools/test_runner.py --image $(IMAGE) tests/integration/

clean:
    rm -rf $(BUILD_DIR)
```

---

## 5. QEMU Launch Script

### tools/qemu-run.sh

```bash
#!/bin/bash
IMAGE=$1
DEBUG_FLAGS=""

if [[ "$2" == "--debug" ]]; then
    DEBUG_FLAGS="-s -S"  # GDB stub on port 1234, wait for connection
fi

qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host,+x2apic,+invpcid,+rdrand,+rdseed,+aes,+sha-ni,+avx2 \
    -smp cores=4,threads=1 \
    -m 4G \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive file="$IMAGE",format=raw,if=none,id=disk0 \
    -device nvme,serial=helios0,drive=disk0 \
    -device virtio-gpu-pci \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-keyboard-pci \
    -device virtio-mouse-pci \
    -serial stdio \
    -monitor telnet:127.0.0.1:55555,server,nowait \
    -d guest_errors,unimp \
    $DEBUG_FLAGS
```

---

## 6. Disk Image Generation

### tools/mkimage.py (outline)

```python
#!/usr/bin/env python3
"""Build a GPT disk image with ESP + Helios object store partition."""

def create_image(args):
    # 1. Create a raw disk image (512 MiB default)
    # 2. Create GPT partition table
    # 3. Partition 1: ESP (FAT32, 64 MiB)
    #    - Copy BOOTX64.EFI to /EFI/HELIOS/
    #    - Copy kernel.bin to /EFI/HELIOS/
    #    - Copy base model (if present) to /EFI/HELIOS/
    # 4. Partition 2: Object store (remaining space)
    #    - Write superblock
    #    - Write initial object graph (seed data)
    #    - Store driver and service binaries as objects
    # 5. Write final image
    pass
```

---

## 7. Cross-Compilation Notes

### 7.1 Building the Cross-Compiler

If the host doesn't have an `x86_64-elf` cross-compiler:

```bash
# Build binutils
./configure --target=x86_64-elf --prefix=$HOME/cross --disable-nls --disable-werror
make && make install

# Build GCC (C only, freestanding)
./configure --target=x86_64-elf --prefix=$HOME/cross \
    --disable-nls --enable-languages=c --without-headers
make all-gcc all-target-libgcc
make install-gcc install-target-libgcc
```

### 7.2 CI Pipeline

```yaml
# .github/workflows/build.yml (conceptual)
name: Helios CI
on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: |
          sudo apt install nasm qemu-system-x86 ovmf mtools dosfstools
          # Install cross-compiler (cached)
      - name: Build
        run: make all
      - name: Unit tests
        run: make test-unit
      - name: Integration tests
        run: make test-integration
```

---

## 8. Kernel Linker Script

### linker/kernel.ld

```ld
/* Helios kernel linker script — maps kernel to upper canonical half */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

KERNEL_VMA = 0xFFFFFFFFFF000000;  /* Virtual base in SASOS kernel region */

SECTIONS {
    . = KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text.entry)      /* kernel_entry must be first */
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
```

---

*Next: [13-ACPI-POWER.md](./13-ACPI-POWER.md) — ACPI Table Parsing & Power Management*
