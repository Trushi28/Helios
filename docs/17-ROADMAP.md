# 17 — Phased Development Roadmap

> **Subsystem:** Project Management  
> **Owner:** All teams  
> **Status:** Planning

---

## 1. Principles

- **Boot early, iterate fast.** Get to "text on screen via serial" in days, not months.
- **Vertical slices.** Each phase delivers a testable, end-to-end capability.
- **QEMU first, bare metal second.** Develop under QEMU/KVM; validate on real hardware periodically.
- **No premature optimization.** Correct first, fast later. But design for performance from the start.

---

## 2. Phase 0 — Foundation (Weeks 1–3)

**Goal:** UEFI boot → kernel entry → "Hello World" on serial console.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 0.1 | Set up cross-compiler toolchain (x86_64-elf-gcc) | ☐ |
| 0.2 | Create project structure (dirs, Makefiles, linker scripts) | ☐ |
| 0.3 | Write UEFI bootloader: `efi_main()` → load kernel → `ExitBootServices()` | ☐ |
| 0.4 | Write kernel entry ASM stub: load GDT, jump to `kernel_main()` | ☐ |
| 0.5 | Implement serial UART output (COM1, 115200 baud) | ☐ |
| 0.6 | Print `boot_info_t` contents to serial (framebuffer, memory map, ACPI) | ☐ |
| 0.7 | Set up QEMU launch script with OVMF | ☐ |
| 0.8 | Set up GDB remote debugging over QEMU stub | ☐ |

### Exit Criteria
- `make run` boots QEMU, prints "Helios kernel alive" to serial console
- GDB can attach, set breakpoints, step through kernel code

---

## 3. Phase 1 — Memory Management (Weeks 4–7)

**Goal:** Physical memory manager + SASOS page tables + capability stubs.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 1.1 | Parse UEFI memory map → identify free regions | ☐ |
| 1.2 | Implement buddy allocator PMM | ☐ |
| 1.3 | Unit test PMM (alloc, free, coalescing) on host | ☐ |
| 1.4 | Build SASOS PML4 page tables | ☐ |
| 1.5 | Map kernel to upper canonical half | ☐ |
| 1.6 | Set up physical memory direct map | ☐ |
| 1.7 | Enable NX bit, PCID, SMEP, SMAP | ☐ |
| 1.8 | Implement slab allocator on top of PMM | ☐ |
| 1.9 | Implement basic capability token create/validate (software-only HMAC) | ☐ |
| 1.10 | Implement IDT + basic exception handlers (#PF, #GP, #DF) | ☐ |
| 1.11 | Implement page fault handler (demand paging) | ☐ |
| 1.12 | Guard page setup around kernel stack | ☐ |

### Exit Criteria
- PMM can allocate/free pages, buddy coalescing works
- Kernel runs in SASOS layout with proper page protection
- `#PF` on null pointer dereference is caught and reported to serial
- Capability tokens can be created and validated

---

## 4. Phase 2 — SMP & Scheduler (Weeks 8–11)

**Goal:** Multi-core bring-up + working scheduler + context switching.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 2.1 | Parse ACPI MADT → enumerate x2APIC IDs | ☐ |
| 2.2 | Initialize BSP x2APIC (MSR-based) | ☐ |
| 2.3 | Write AP trampoline (real → long mode) | ☐ |
| 2.4 | INIT-SIPI-SIPI sequence → bring up all APs | ☐ |
| 2.5 | Per-core GS base → per-core data structures | ☐ |
| 2.6 | TSC calibration (CPUID leaf 0x15 + PIT fallback) | ☐ |
| 2.7 | x2APIC one-shot timer for preemption | ☐ |
| 2.8 | Implement micro-program control block | ☐ |
| 2.9 | Implement per-core O(1) scheduler with priority bitmap | ☐ |
| 2.10 | Context switch (save/restore GPRs + XSAVE) | ☐ |
| 2.11 | Work stealing between cores | ☐ |
| 2.12 | IPI vectors (reschedule, TLB shootdown) | ☐ |
| 2.13 | SYSCALL/SYSRET entry point for user-space | ☐ |
| 2.14 | `sys_spawn`, `sys_exit`, `sys_yield` syscalls | ☐ |
| 2.15 | Spawn test micro-programs that print to serial from Ring 3 | ☐ |

### Exit Criteria
- All CPU cores active and running scheduler
- Multiple micro-programs scheduled and preempted across cores
- Context switch measured at < 500 ns (target: ~180 ns)
- Work stealing verified under asymmetric load

---

## 5. Phase 3 — Storage & Object Graph (Weeks 12–17)

**Goal:** NVMe driver + content-addressed object store + graph queries.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 3.1 | Parse ACPI MCFG → map PCIe ECAM | ☐ |
| 3.2 | PCIe device enumeration | ☐ |
| 3.3 | Parse PCI BARs and capabilities | ☐ |
| 3.4 | MSI-X configuration for PCIe devices | ☐ |
| 3.5 | IOMMU (VT-d) initialization from DMAR | ☐ |
| 3.6 | NVMe controller initialization (admin queue) | ☐ |
| 3.7 | NVMe I/O queue creation (per-core) | ☐ |
| 3.8 | NVMe read/write (sync + async) | ☐ |
| 3.9 | Driver isolation: NVMe as user-space micro-program | ☐ |
| 3.10 | Implement SHA-256 (with SHA-NI acceleration) | ☐ |
| 3.11 | Object store: superblock, extent bitmap | ☐ |
| 3.12 | Object store: `obj_store()`, `obj_load()`, content verification | ☐ |
| 3.13 | Vertex and edge tables | ☐ |
| 3.14 | Transaction journal (append-only WAL) | ☐ |
| 3.15 | Graph query: `sys_vertex_create`, `sys_edge_create`, `sys_query_vertices` | ☐ |
| 3.16 | Snapshot creation and listing | ☐ |
| 3.17 | In-memory object/vertex cache (LRU) | ☐ |
| 3.18 | `mkimage.py` tool: create GPT image with ESP + object store partition | ☐ |

### Exit Criteria
- NVMe reads/writes working at near-bandwidth speed
- Objects stored and retrieved with SHA-256 verification
- Graph queries return correct results
- Snapshot creates a restorable point-in-time view

---

## 6. Phase 4 — GPU Compositor (Weeks 18–23)

**Goal:** GPU-accelerated display with text rendering and the data matrix.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 4.1 | GOP framebuffer: render text (bitmap font) during early boot | ☐ |
| 4.2 | Virtio-GPU driver: device init, command queues | ☐ |
| 4.3 | GPU resource creation (2D framebuffer) | ☐ |
| 4.4 | Implement data matrix structure in shared SASOS memory | ☐ |
| 4.5 | Implement compositor render loop (scan dirty → draw → present) | ☐ |
| 4.6 | Glyph atlas: rasterize embedded font → GPU texture | ☐ |
| 4.7 | Text rendering: matrix cell → glyph quads → framebuffer | ☐ |
| 4.8 | Implement `sys_ui_claim_region`, `sys_ui_write_cell` syscalls | ☐ |
| 4.9 | Tiling layout shader (default structural shader) | ☐ |
| 4.10 | Cursor rendering and input focus model | ☐ |
| 4.11 | Basic animation system (cell transitions) | ☐ |
| 4.12 | Display mode configuration (`sys_display_set_mode`) | ☐ |
| 4.13 | Color theming (dark mode as default) | ☐ |

### Exit Criteria
- Multiple regions visible on screen simultaneously (tiled)
- Text renders clearly with anti-aliased glyph atlas
- Compositor holds 60 FPS with active terminal output
- Input cursor visible and blinking

---

## 7. Phase 5 — Intelligence Layer (Weeks 24–27)

**Goal:** NPU memory enclave + sys_infer + AI completions.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 5.1 | Reserve NPU enclave physical memory during boot | ☐ |
| 5.2 | Map enclave into SASOS (kernel-only pages) | ☐ |
| 5.3 | Implement GGUF model loader (parse header, mmap weights) | ☐ |
| 5.4 | Implement BPE tokenizer | ☐ |
| 5.5 | CPU inference backend: AVX2/AVX-512 quantized matmul | ☐ |
| 5.6 | Transformer forward pass (attention, FFN, softmax, sampling) | ☐ |
| 5.7 | KV cache management (allocate, reuse, evict) | ☐ |
| 5.8 | Inference request queue + scheduler | ☐ |
| 5.9 | `sys_infer` syscall (sync + async) | ☐ |
| 5.10 | `sys_embed` syscall for embedding vectors | ☐ |
| 5.11 | Inference resource quotas per micro-program | ☐ |
| 5.12 | Benchmark: measure tokens/sec on QEMU (CPU backend) | ☐ |

### Exit Criteria
- A 1B-parameter quantized model runs inference from kernel space
- `sys_infer` returns coherent text completions
- Multiple micro-programs can submit concurrent inference requests
- Token generation > 5 tok/s on CPU (emulated; much faster on bare metal)

---

## 8. Phase 6 — Shell & User Space (Weeks 28–31)

**Goal:** Interactive shell with graph queries, AI completions, and pipeline execution.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 6.1 | Minimal libc for micro-programs (string, stdio, stdlib) | ☐ |
| 6.2 | libhelios syscall wrappers | ☐ |
| 6.3 | Micro-program CRT0 entry point | ☐ |
| 6.4 | Shell micro-program: line editor + history | ☐ |
| 6.5 | Graph query parser | ☐ |
| 6.6 | Built-in commands: `ls`, `cd`, `cat`, `ps`, `stat` | ☐ |
| 6.7 | Pipeline executor: `query | sort | head` | ☐ |
| 6.8 | AI autocomplete integration (`sys_infer` on partial input) | ☐ |
| 6.9 | Natural language → graph query translation | ☐ |
| 6.10 | Syntax highlighting in shell | ☐ |
| 6.11 | Shell configuration stored as graph object | ☐ |
| 6.12 | Basic text editor micro-program | ☐ |

### Exit Criteria
- User can type commands, navigate the object graph, create/read objects
- AI completions appear in < 500 ms (CPU backend)
- Pipeline `@tagged(kernel) | sort size desc | head 10` works end-to-end
- Text editor can modify objects (creates new immutable versions)

---

## 9. Phase 7 — Networking, USB & Audio (Weeks 32–37)

**Goal:** TCP/IP connectivity + USB HID + audio output.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 7.1 | Virtio-net driver (or E1000e for VM compatibility) | ☐ |
| 7.2 | Zero-copy packet pool | ☐ |
| 7.3 | ARP + IPv4 stack | ☐ |
| 7.4 | UDP | ☐ |
| 7.5 | TCP (connection, data transfer, teardown) | ☐ |
| 7.6 | DHCP client | ☐ |
| 7.7 | DNS resolver | ☐ |
| 7.8 | Network API (IPC-based, capability-mediated) | ☐ |
| 7.9 | xHCI host controller driver | ☐ |
| 7.10 | USB device enumeration | ☐ |
| 7.11 | HID keyboard + mouse drivers | ☐ |
| 7.12 | Input event → compositor routing | ☐ |
| 7.13 | Intel HDA controller driver | ☐ |
| 7.14 | Audio stream setup + DMA playback | ☐ |
| 7.15 | Software audio mixer | ☐ |
| 7.16 | Audio API (`sys_audio_open`, `sys_audio_write`) | ☐ |

### Exit Criteria
- `ping` equivalent works over the network
- Keyboard and mouse input drives the shell/compositor
- System boot chime plays through audio
- TCP connection to an external server transfers data

---

## 10. Phase 8 — Polish & Bare Metal (Weeks 38+)

**Goal:** Bare-metal validation, performance tuning, robustness.

### Deliverables

| # | Task | Status |
|---|------|--------|
| 8.1 | Boot on real hardware (test machine with NVMe + discrete GPU) | ☐ |
| 8.2 | Debug and fix hardware-specific issues | ☐ |
| 8.3 | Intel/AMD GPU driver (basic modesetting + framebuffer) | ☐ |
| 8.4 | Performance profiling (PMC-based, per-core) | ☐ |
| 8.5 | Stress test: memory allocator under heavy load | ☐ |
| 8.6 | Stress test: IPC throughput with 100+ micro-programs | ☐ |
| 8.7 | Stress test: NVMe I/O under heavy concurrent load | ☐ |
| 8.8 | Secure boot chain: sign kernel + drivers, verify on boot | ☐ |
| 8.9 | TPM 2.0 integration (if hardware available) | ☐ |
| 8.10 | Garbage collector for object store | ☐ |
| 8.11 | Multiple structural shader presets (stack, radial, 3D ring) | ☐ |
| 8.12 | System installer (create bootable USB from running system) | ☐ |
| 8.13 | GPU compute backend for inference (Vulkan compute shaders) | ☐ |
| 8.14 | IPv6 support | ☐ |
| 8.15 | TLS 1.3 (for HTTPS) | ☐ |
| 8.16 | Multi-monitor support | ☐ |
| 8.17 | Power management: suspend (S3), hibernate (S4) | ☐ |
| 8.18 | ACPI thermal management on real hardware | ☐ |
| 8.19 | Documentation: user guide, developer guide | ☐ |
| 8.20 | Automated CI: build + QEMU integration tests on every commit | ☐ |

### Exit Criteria
- System boots on at least one real x86-64 machine from NVMe
- All subsystems functional on bare metal
- No kernel panics under 24-hour stress test
- Performance within 2x of design targets

---

## 11. Future Roadmap (Post-v1.0)

| Feature | Description |
|---------|-------------|
| ARM64 port | AArch64 support (Raspberry Pi 5, server ARM) |
| RISC-V port | RISC-V 64-bit support |
| Wayland compatibility shim | Run existing Wayland apps via translation layer |
| Package manager | Install/update micro-programs from a repository |
| Encrypted object store | AES-256-GCM at-rest encryption |
| Remote object graph sync | Distributed graph replication between machines |
| Voice interface | Microphone input → sys_infer → voice commands |
| Hardware debugger | Built-in hardware debugger (GDB-server in kernel) |
| Formal verification | Verify capability system invariants with Coq/Lean |

---

## 12. Estimated Timeline Summary

```
Month 1     ████████  Phase 0 + Phase 1 (Boot + Memory)
Month 2     ████████  Phase 1 + Phase 2 (Memory + SMP)
Month 3     ████████  Phase 2 + Phase 3 (SMP + Storage)
Month 4     ████████  Phase 3 (Storage + Object Graph)
Month 5     ████████  Phase 4 (GPU Compositor)
Month 6     ████████  Phase 4 + Phase 5 (Compositor + Intelligence)
Month 7     ████████  Phase 5 + Phase 6 (Intelligence + Shell)
Month 8     ████████  Phase 6 + Phase 7 (Shell + Networking)
Month 9     ████████  Phase 7 (USB + Audio)
Month 10+   ████████  Phase 8 (Polish + Bare Metal)
```

---

*This concludes the Helios OS design specification. Return to [00-OVERVIEW.md](./00-OVERVIEW.md).*
