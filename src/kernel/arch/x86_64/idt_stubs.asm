; ═══════════════════════════════════════════════════════════════════════════════
; Helios IDT Stub Assembly
; ═══════════════════════════════════════════════════════════════════════════════
;
; Provides ISR entry stubs for all CPU exception vectors (0–31) and a generic
; stub for vectors 32–255. Each stub pushes a uniform interrupt frame onto the
; stack and calls the C function interrupt_dispatch().
; ═══════════════════════════════════════════════════════════════════════════════

[bits 64]
section .text

extern interrupt_dispatch

; ── Macro: ISR without hardware error code ──────────────────────────────────
; The CPU does not push an error code for these exceptions, so we push
; a dummy 0 to keep the stack frame uniform.
%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push qword 0              ; Dummy error code
    push qword %1             ; Interrupt number
    jmp isr_common
%endmacro

; ── Macro: ISR with hardware error code ─────────────────────────────────────
; The CPU already pushed the error code, so we only push the vector number.
%macro ISR_ERR 1
global isr_%1
isr_%1:
    push qword %1             ; Interrupt number (error code already on stack)
    jmp isr_common
%endmacro

; ═══════════════════════════════════════════════════════════════════════════════
; Exception vectors 0–31
; ═══════════════════════════════════════════════════════════════════════════════

ISR_NOERR 0    ; #DE Divide Error
ISR_NOERR 1    ; #DB Debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; #BP Breakpoint
ISR_NOERR 4    ; #OF Overflow
ISR_NOERR 5    ; #BR Bound Range Exceeded
ISR_NOERR 6    ; #UD Invalid Opcode
ISR_NOERR 7    ; #NM Device Not Available
ISR_ERR   8    ; #DF Double Fault (error code = 0)
ISR_NOERR 9    ; Coprocessor Segment Overrun (reserved)
ISR_ERR   10   ; #TS Invalid TSS
ISR_ERR   11   ; #NP Segment Not Present
ISR_ERR   12   ; #SS Stack-Segment Fault
ISR_ERR   13   ; #GP General Protection Fault
ISR_ERR   14   ; #PF Page Fault
ISR_NOERR 15   ; (Reserved)
ISR_NOERR 16   ; #MF x87 FP Exception
ISR_ERR   17   ; #AC Alignment Check
ISR_NOERR 18   ; #MC Machine Check
ISR_NOERR 19   ; #XM SIMD FP Exception
ISR_NOERR 20   ; #VE Virtualization Exception
ISR_ERR   21   ; #CP Control Protection
ISR_NOERR 22   ; (Reserved)
ISR_NOERR 23   ; (Reserved)
ISR_NOERR 24   ; (Reserved)
ISR_NOERR 25   ; (Reserved)
ISR_NOERR 26   ; (Reserved)
ISR_NOERR 27   ; (Reserved)
ISR_NOERR 28   ; #HV Hypervisor Injection
ISR_NOERR 29   ; #VC VMM Communication
ISR_ERR   30   ; #SX Security Exception
ISR_NOERR 31   ; (Reserved)

; ═══════════════════════════════════════════════════════════════════════════════
; Generic ISR stub for vectors 32–255 (device interrupts, IPIs, etc.)
; ═══════════════════════════════════════════════════════════════════════════════

global isr_generic
isr_generic:
    push qword 0              ; Dummy error code
    push qword 0xFF           ; Marker: "unknown/generic vector"
    jmp isr_common

; ═══════════════════════════════════════════════════════════════════════════════
; Common ISR body
; ═══════════════════════════════════════════════════════════════════════════════
; Stack at entry to isr_common (bottom → top):
;   [SS, RSP, RFLAGS, CS, RIP] — pushed by CPU
;   [error_code]                — pushed by CPU or ISR_NOERR macro
;   [int_no]                    — pushed by ISR macro
;
; We now push all general-purpose registers to form a complete interrupt_frame_t.

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

    ; Pass RSP as argument → pointer to interrupt_frame_t
    mov rdi, rsp
    call interrupt_dispatch

    ; Restore all general-purpose registers
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

    ; Remove int_no and error_code from stack
    add rsp, 16

    ; Return from interrupt
    iretq
