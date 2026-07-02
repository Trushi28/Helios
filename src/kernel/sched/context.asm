; ═══════════════════════════════════════════════════════════════════════════════
; Helios — context_switch(cpu_context_t *save_to, cpu_context_t *restore_from)
; ═══════════════════════════════════════════════════════════════════════════════
;
; Saves callee-saved registers to save_to->rsp, restores from restore_from->rsp.
;
; cpu_context_t layout (offsets):
;   rsp = 0, rbp = 8, rbx = 16, r12 = 24, r13 = 32, r14 = 40, r15 = 48
;
; The stack of the saved task will contain (top to bottom):
;   [return address]  ← pushed by the `call context_switch` instruction
;   [r15] [r14] [r13] [r12] [rbx] [rbp]  ← pushed here
;
; For a newly created task, microprog_create() pre-populates the stack
; with the same layout, so the first context_switch to it works correctly.
; ═══════════════════════════════════════════════════════════════════════════════

    [bits 64]
    section .text

    global context_switch

; void context_switch(cpu_context_t *save_to,      ; RDI
;                     cpu_context_t *restore_from); ; RSI
context_switch:
    ; ── Save current context ──────────────────────────────────────────────
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save RSP into save_to->rsp (offset 0)
    mov [rdi], rsp

    ; ── Restore new context ───────────────────────────────────────────────
    ; Load RSP from restore_from->rsp (offset 0)
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Return to the new task's saved return address (or entry_rip for new tasks)
    ret
