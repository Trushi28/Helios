# 14 — IDT, x2APIC & MSI/MSI-X Interrupt Architecture

> **Subsystem:** Interrupts  
> **Owner:** Kernel team  
> **Dependencies:** x2APIC, I/O APIC, PCIe MSI-X, IDT  
> **Related:** [01-BOOT.md](./01-BOOT.md), [03-SCHEDULER.md](./03-SCHEDULER.md), [07-DRIVERS.md](./07-DRIVERS.md)

---

## 1. Design Philosophy

Helios uses an exclusively modern interrupt architecture:

- **x2APIC** for local interrupt control (MSR-based, no MMIO)
- **I/O APIC** only for legacy ISA remapping (keyboard during early USB init)
- **MSI/MSI-X** for all PCIe device interrupts (NVMe, GPU, NIC, xHCI)
- **No PIC (8259A)**. The legacy PIC is disabled during boot.

---

## 2. Interrupt Descriptor Table (IDT)

### 2.1 Layout

The IDT has 256 entries. Helios partitions them as follows:

| Vector Range | Purpose |
|-------------|---------|
| 0x00 – 0x1F | CPU Exceptions (reserved by Intel) |
| 0x20 – 0x2F | Legacy ISA interrupts (I/O APIC remapped, temporary) |
| 0x30 – 0xBF | MSI/MSI-X device interrupts (dynamic allocation) |
| 0xC0 – 0xDF | Reserved for future use |
| 0xE0 – 0xEF | Syscall vectors |
| 0xF0 – 0xF3 | IPI vectors (reschedule, TLB shootdown, halt, panic) |
| 0xF4 – 0xFD | Reserved |
| 0xFE | x2APIC spurious interrupt |
| 0xFF | Reserved |

### 2.2 IDT Entry Format

```c
typedef struct {
    uint16_t    offset_low;         // Bits 0–15 of handler address
    uint16_t    segment;            // Code segment selector (0x08)
    uint8_t     ist;                // Interrupt Stack Table index (0–7)
    uint8_t     type_attr;          // Gate type + DPL + present
    uint16_t    offset_mid;         // Bits 16–31
    uint32_t    offset_high;        // Bits 32–63
    uint32_t    _reserved;
} __attribute__((packed)) idt_entry_t;

_Static_assert(sizeof(idt_entry_t) == 16, "IDT entry must be 16 bytes");

typedef struct {
    uint16_t    limit;
    uint64_t    base;
} __attribute__((packed)) idt_ptr_t;

extern idt_entry_t g_idt[256];
extern idt_ptr_t   g_idt_ptr;
```

### 2.3 Gate Types

| Type | Usage |
|------|-------|
| Interrupt Gate (0x8E) | Standard interrupt — clears IF (disables interrupts) |
| Trap Gate (0x8F) | Exceptions — does not clear IF |
| Interrupt Gate + DPL=3 (0xEE) | Syscall entry — callable from Ring 3 |

### 2.4 Interrupt Stack Table (IST)

Critical interrupts use dedicated stacks to handle corner cases (e.g., stack overflow causing a double fault on the overflowed stack):

| IST | Stack Size | Usage |
|-----|-----------|-------|
| IST 1 | 16 KiB | Double Fault (#DF) |
| IST 2 | 8 KiB | NMI |
| IST 3 | 8 KiB | Machine Check (#MC) |
| IST 4 | 8 KiB | Debug exceptions (#DB, #BP) |

```c
// Configure IST in the per-core TSS
typedef struct {
    uint32_t    _reserved0;
    uint64_t    rsp0;               // Ring 0 stack (for Ring 3 → Ring 0 transitions)
    uint64_t    rsp1;               // Ring 1 stack (unused)
    uint64_t    rsp2;               // Ring 2 stack (unused)
    uint64_t    _reserved1;
    uint64_t    ist[7];             // Interrupt Stack Table entries 1–7
    uint64_t    _reserved2;
    uint16_t    _reserved3;
    uint16_t    iopb_offset;        // I/O Permission Bitmap offset
} __attribute__((packed)) tss_t;
```

---

## 3. CPU Exception Handlers

### 3.1 Exception Table

| Vector | Mnemonic | Description | Error Code? | Handler |
|--------|----------|-------------|-------------|---------|
| 0x00 | #DE | Divide Error | No | Kill µP |
| 0x01 | #DB | Debug | No | Debug handler |
| 0x02 | NMI | Non-Maskable Interrupt | No | NMI handler (IST 2) |
| 0x03 | #BP | Breakpoint | No | Debug handler (IST 4) |
| 0x04 | #OF | Overflow | No | Kill µP |
| 0x05 | #BR | Bound Range Exceeded | No | Kill µP |
| 0x06 | #UD | Invalid Opcode | No | Kill µP |
| 0x07 | #NM | Device Not Available | No | FPU lazy state restore |
| 0x08 | #DF | Double Fault | Yes (0) | Panic (IST 1) |
| 0x0A | #TS | Invalid TSS | Yes | Panic |
| 0x0B | #NP | Segment Not Present | Yes | Panic |
| 0x0C | #SS | Stack-Segment Fault | Yes | Kill µP |
| 0x0D | #GP | General Protection | Yes | Cap violation / Kill µP |
| 0x0E | #PF | Page Fault | Yes | Page fault handler |
| 0x10 | #MF | x87 FP Exception | No | Kill µP |
| 0x11 | #AC | Alignment Check | Yes | Kill µP |
| 0x12 | #MC | Machine Check | No | Panic (IST 3) |
| 0x13 | #XM | SIMD FP Exception | No | Kill µP |
| 0x1E | #SX | Security Exception | Yes | Panic |

### 3.2 Common Exception Entry

All exception handlers share a common ASM stub:

```nasm
; Common interrupt/exception entry stub
; Saves all registers, calls C handler, restores, IRETQ

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push 0              ; Dummy error code
    push %1             ; Interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    push %1             ; Interrupt number (error code already pushed by CPU)
    jmp isr_common
%endmacro

isr_common:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass stack pointer as argument (points to interrupt_frame_t)
    mov rdi, rsp
    call interrupt_dispatch      ; C function

    ; Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; Remove error code + interrupt number
    iretq
```

### 3.3 Interrupt Frame

```c
typedef struct {
    // Pushed by isr_common (in reverse order)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Pushed by ISR stub
    uint64_t int_no;
    uint64_t error_code;

    // Pushed by CPU on interrupt
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;
```

### 3.4 Dispatch

```c
void interrupt_dispatch(interrupt_frame_t *frame) {
    uint64_t vector = frame->int_no;

    if (vector < 0x20) {
        // CPU exception
        exception_handlers[vector](frame);
    } else if (vector >= 0x30 && vector <= 0xBF) {
        // Device interrupt (MSI/MSI-X)
        device_irq_handler(vector, frame);
        x2apic_eoi();
    } else if (vector >= 0xF0 && vector <= 0xF3) {
        // IPI
        ipi_handlers[vector - 0xF0](frame);
        x2apic_eoi();
    } else if (vector == 0xFE) {
        // Spurious — do nothing, no EOI
    } else {
        // Unknown vector
        serial_printf("Unknown interrupt vector: 0x%02X\n", vector);
        x2apic_eoi();
    }
}
```

---

## 4. I/O APIC

### 4.1 Configuration

The I/O APIC is used minimally — only for ISA IRQ redirection during early boot (before USB HID is initialized):

```c
typedef struct {
    uint32_t    id;
    uint64_t    mmio_base_phys;
    uint64_t    mmio_base_virt;     // Mapped into SASOS MMIO region
    uint32_t    gsi_base;           // Global System Interrupt base
    uint32_t    max_redirection;    // Number of redirection entries
} ioapic_t;

// I/O APIC registers (accessed via IOREGSEL/IOWIN)
void ioapic_write(ioapic_t *apic, uint8_t reg, uint32_t value);
uint32_t ioapic_read(ioapic_t *apic, uint8_t reg);

// Redirection table entry
typedef struct {
    uint64_t    vector          : 8;
    uint64_t    delivery_mode   : 3;    // 0=Fixed, 1=LowPri, 2=SMI, 4=NMI, 5=INIT, 7=ExtINT
    uint64_t    dest_mode       : 1;    // 0=Physical, 1=Logical
    uint64_t    delivery_status : 1;
    uint64_t    pin_polarity    : 1;    // 0=Active High, 1=Active Low
    uint64_t    remote_irr      : 1;
    uint64_t    trigger_mode    : 1;    // 0=Edge, 1=Level
    uint64_t    mask            : 1;    // 1=Masked
    uint64_t    _reserved       : 39;
    uint64_t    destination     : 8;    // APIC ID (physical mode)
} ioapic_rte_t;
```

### 4.2 ISA IRQ Overrides

The MADT contains Interrupt Source Override entries that remap ISA IRQs to GSIs:

```c
// Default: ISA IRQ N → GSI N (identity mapping)
// Overrides: e.g., IRQ 0 (PIT) → GSI 2, IRQ 9 → GSI 9 (level-triggered)

typedef struct {
    uint8_t     bus_source;     // Always 0 (ISA)
    uint8_t     irq_source;     // ISA IRQ number
    uint32_t    gsi;            // Global System Interrupt
    uint16_t    flags;          // Polarity + trigger mode
} irq_override_t;

extern irq_override_t g_irq_overrides[16];
extern uint32_t       g_irq_override_count;
```

---

## 5. MSI / MSI-X

### 5.1 Why MSI-X Over Legacy IRQs

| Feature | Legacy (I/O APIC) | MSI-X |
|---------|-------------------|-------|
| Interrupt vectors per device | 1 | Up to 2048 |
| Requires I/O APIC wiring | Yes | No (direct CPU delivery) |
| Per-core targeting | Indirect (logical mode) | Direct (x2APIC ID in address) |
| Shared IRQ lines | Yes (problematic) | No (dedicated vectors) |

### 5.2 MSI-X Configuration

```c
typedef struct {
    // MSI-X Table Entry (in device BAR)
    uint64_t    msg_addr;       // Message address (encodes target APIC + vector)
    uint32_t    msg_data;       // Message data (vector number)
    uint32_t    vector_ctrl;    // Bit 0: mask (1=masked)
} msix_table_entry_t;

void msix_configure(pcie_device_t *dev, uint16_t entry_index,
                    uint8_t vector, uint32_t target_apic_id) {
    // MSI-X message address format (x2APIC):
    // [63:32] = upper destination APIC ID bits
    // [31:20] = 0xFEE (fixed)
    // [19:12] = destination APIC ID [7:0]
    // [11:4]  = 0
    // [3]     = redirect hint (0)
    // [2]     = destination mode (0=physical)
    // [1:0]   = 0

    uint64_t addr = 0xFEE00000ULL |
                    ((uint64_t)(target_apic_id & 0xFF) << 12) |
                    ((uint64_t)(target_apic_id >> 8) << 32);

    // MSI-X message data format:
    // [7:0] = vector number
    // Other bits = delivery mode, trigger mode (edge)
    uint32_t data = vector;

    // Write to MSI-X table (mapped via BAR)
    volatile msix_table_entry_t *table = msix_get_table(dev);
    table[entry_index].msg_addr = addr;
    table[entry_index].msg_data = data;
    table[entry_index].vector_ctrl = 0;  // Unmask
}
```

### 5.3 Dynamic Vector Allocation

```c
#define IRQ_VECTOR_BASE     0x30
#define IRQ_VECTOR_MAX      0xBF
#define IRQ_VECTOR_COUNT    (IRQ_VECTOR_MAX - IRQ_VECTOR_BASE + 1)

typedef struct {
    uint8_t             vector;
    uint32_t            owner_mprog_id;     // Driver micro-program
    uint64_t            notify_port;        // IPC port to notify on IRQ
    pcie_device_t      *device;
    void              (*kernel_handler)(interrupt_frame_t *);  // For kernel-level IRQs
} irq_binding_t;

extern irq_binding_t g_irq_bindings[IRQ_VECTOR_COUNT];
static uint8_t        g_next_vector = IRQ_VECTOR_BASE;

uint8_t irq_alloc_vector(void) {
    if (g_next_vector > IRQ_VECTOR_MAX) {
        panic("IRQ vector exhaustion");
    }
    return g_next_vector++;
}
```

---

## 6. Device Interrupt Delivery to Drivers

Since drivers are user-space micro-programs, hardware interrupts are converted to IPC messages:

```c
void device_irq_handler(uint64_t vector, interrupt_frame_t *frame) {
    uint32_t idx = vector - IRQ_VECTOR_BASE;
    irq_binding_t *binding = &g_irq_bindings[idx];

    if (binding->kernel_handler) {
        // Kernel-level handler (e.g., x2APIC timer)
        binding->kernel_handler(frame);
    } else if (binding->notify_port) {
        // Send interrupt notification to driver via IPC
        ipc_message_t msg = {
            .msg_type = IPC_MSG_TYPE_IRQ,
            .flags = 0,
            .sender_mprog = 0,  // Kernel
            .inline_len = sizeof(uint64_t),
        };
        *(uint64_t *)msg.inline_data = vector;
        ipc_send_from_irq(binding->notify_port, &msg);

        // If driver is blocked on ipc_recv, wake it immediately
        scheduler_wake_port_waiter(binding->notify_port);
    }
}
```

### 6.1 EOI (End of Interrupt)

```c
static inline void x2apic_eoi(void) {
    wrmsr(MSR_X2APIC_EOI, 0);
}
```

---

## 7. x2APIC Timer

Used for scheduler preemption (see [03-SCHEDULER.md](./03-SCHEDULER.md)):

```c
#define TIMER_VECTOR    0x20    // Timer fires on this vector

void x2apic_timer_init(void) {
    // Set LVT Timer entry: one-shot mode, vector TIMER_VECTOR
    wrmsr(MSR_X2APIC_LVT_TIMER, TIMER_VECTOR);  // No PERIODIC bit = one-shot

    // Set divider to 1 (highest resolution)
    wrmsr(MSR_X2APIC_TIMER_DIV, 0x0B);  // Divide by 1

    // Timer will be armed by scheduler_arm_timer() when dispatching a µP
}

void timer_interrupt_handler(interrupt_frame_t *frame) {
    // Timer expired → preempt current micro-program
    scheduler_preempt(frame);
}
```

---

## 8. NMI Handling

```c
void nmi_handler(interrupt_frame_t *frame) {
    // NMI sources:
    // 1. Hardware failure (memory parity error)
    // 2. Watchdog timer expiry
    // 3. External NMI button (debug)

    // Read NMI status from port 0x61
    uint8_t reason = inb(0x61);

    if (reason & 0x80) {
        // Memory parity error
        serial_puts("NMI: Memory parity error!\n");
        panic_with_frame("NMI: memory parity error", frame);
    } else if (reason & 0x40) {
        // I/O channel check
        serial_puts("NMI: I/O channel check!\n");
        panic_with_frame("NMI: I/O channel check", frame);
    } else {
        // Software NMI or watchdog
        serial_puts("NMI: Software/watchdog\n");
        // Dump registers for debugging, continue
        debug_dump_frame(frame);
    }
}
```

---

*Next: [15-USB.md](./15-USB.md) — xHCI USB 3.x Host Controller & HID Input*
