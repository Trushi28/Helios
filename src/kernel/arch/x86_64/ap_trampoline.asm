; ═══════════════════════════════════════════════════════════════════════════════
; Helios — AP Trampoline (16-bit real → 32-bit protected → 64-bit long mode)
; ═══════════════════════════════════════════════════════════════════════════════
;
; Loaded at physical 0x8000 by smp_init(). Each AP starts execution here
; via SIPI (vector = 0x08 → physical 0x8000).
;
; Data block at AP_DATA_OFFSET (0x0F00 from base) is populated by smp_init():
;   +0x00  uint64_t  cr3            ; PML4 physical address
;   +0x08  uint64_t  stack_top      ; per-AP kernel stack top
;   +0x10  uint64_t  entry_addr     ; C function pointer (ap_entry)
;   +0x18  uint64_t  gdt_ptr        ; 10-byte GDTR (limit+base)
;   +0x28  uint32_t  ap_ready       ; flag set by AP when ready (AP writes 1)
;   +0x2C  uint32_t  ap_index       ; logical AP index
; ═══════════════════════════════════════════════════════════════════════════════

%define AP_DATA_OFFSET 0x0F00

; The trampoline is assembled as a flat binary (nasm -f bin).
; All addresses are relative to the 0x8000 base.
[bits 16]
[org 0x8000]

ap_start:
    cli
    cld

    ; Set up 16-bit segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load a temporary GDT for the transition
    lgdt [0x8000 + AP_DATA_OFFSET + 0x18]

    ; Enable protected mode (PE bit in CR0)
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to 32-bit protected mode (selector 0x08 = code)
    jmp dword 0x08:(0x8000 + ap_pm32)

[bits 32]
ap_pm32:
    ; Set data segments to selector 0x10
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Enable PAE (CR4.PAE = bit 5)
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load PML4 from data block
    mov eax, [0x8000 + AP_DATA_OFFSET + 0x00]
    mov cr3, eax

    ; Enable long mode (EFER.LME = bit 8)
    mov ecx, 0xC0000080    ; MSR_IA32_EFER
    rdmsr
    or eax, (1 << 8)       ; LME
    or eax, (1 << 11)      ; NXE
    wrmsr

    ; Enable paging (CR0.PG = bit 31)
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; Far jump to 64-bit long mode
    jmp dword 0x08:(0x8000 + ap_lm64)

[bits 64]
ap_lm64:
    ; Reload data segments for 64-bit mode
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Load the kernel's GDT (replaces the temporary trampoline GDT)
    lgdt [0x8000 + AP_DATA_OFFSET + 0x18]

    ; Set up the per-AP stack
    mov rsp, [0x8000 + AP_DATA_OFFSET + 0x08]

    ; Pass ap_index in RDI (first argument to ap_entry)
    xor rdi, rdi
    mov edi, [0x8000 + AP_DATA_OFFSET + 0x2C]

    ; Jump to the C entry point
    mov rax, [0x8000 + AP_DATA_OFFSET + 0x10]
    call rax

    ; Should never return — halt if it does
    cli
.spin:
    hlt
    jmp .spin

; ═══════════════════════════════════════════════════════════════════════════════
; Trampoline size marker (for memcpy in smp_init)
; ═══════════════════════════════════════════════════════════════════════════════
ap_trampoline_end:
