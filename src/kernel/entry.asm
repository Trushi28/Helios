; ═══════════════════════════════════════════════════════════════════════════════
; Helios Kernel Entry Point
; ═══════════════════════════════════════════════════════════════════════════════
;
; Called by the UEFI bootloader after ExitBootServices().
; CPU state on entry:
;   RDI = physical address of boot_info_t
;   RSI = 0 (reserved)
;   Mode: 64-bit long mode, paging active (UEFI identity map)
;   Interrupts: disabled (CLI)
;
; This stub:
;   1. Sets up an initial kernel stack
;   2. Installs the GDT
;   3. Calls kernel_main(boot_info_t *boot_info)
; ═══════════════════════════════════════════════════════════════════════════════

[bits 64]
section .text.entry

global kernel_entry
extern kernel_main
extern gdt_install
extern idt_install

kernel_entry:
    ; ── Disable interrupts (should already be disabled) ──────────────────
    cli

    ; ── Save boot_info pointer (RDI) ────────────────────────────────────
    ; RDI is preserved as the first argument to kernel_main per SysV ABI.
    push rdi

    ; ── Set up initial kernel stack ──────────────────────────────────────
    ; Use a statically allocated 64 KiB stack in BSS.
    lea rsp, [rel kernel_stack_top]

    ; ── Restore boot_info pointer ────────────────────────────────────────
    pop rdi
    push rdi                    ; Keep it on the new stack too

    ; ── Zero the BSS section ─────────────────────────────────────────────
    ; Note: we must be careful not to zero our own stack (which is in BSS).
    ; The BSS is zeroed by the kernel build (objcopy flat binary) but we
    ; do it explicitly for safety. We'll skip the stack region.
    ; For Phase 0, we rely on the loader zeroing BSS in the flat binary.

    ; ── Install GDT ─────────────────────────────────────────────────────
    call gdt_install

    ; ── Install IDT ─────────────────────────────────────────────────────
    call idt_install

    ; ── Call kernel_main(boot_info_t *boot_info) ─────────────────────────
    pop rdi                     ; boot_info_t* in RDI
    xor rbp, rbp                ; Clear frame pointer for stack traces
    call kernel_main

    ; ── Should never return, but halt if it does ─────────────────────────
.hang:
    cli
    hlt
    jmp .hang

; ═══════════════════════════════════════════════════════════════════════════════
; Kernel Stack (64 KiB, in BSS)
; ═══════════════════════════════════════════════════════════════════════════════
section .bss
align 16

kernel_stack_bottom:
    resb 65536              ; 64 KiB stack
kernel_stack_top:
