; ═══════════════════════════════════════════════════════════════════════════════
; Helios — SYSCALL entry stub
; ═══════════════════════════════════════════════════════════════════════════════
;
; When SYSCALL executes, the CPU:
;   RCX ← RIP of instruction after SYSCALL (return address)
;   R11 ← RFLAGS
;   RIP ← LSTAR (this stub)
;   CS  ← STAR[47:32]     = 0x08 (kernel code)
;   SS  ← STAR[47:32] + 8 = 0x10 (kernel data)
;   RFLAGS &= ~SFMASK      (IF, DF, TF cleared)
;
; Register convention for syscalls:
;   RAX = syscall number
;   RDI = arg1, RSI = arg2, RDX = arg3
;   Return value in RAX
;
; Per-core data layout (per_core.h):
;   [gs:0]  = self pointer
;   [gs:8]  = kernel_stack_top
;   [gs:16] = user_rsp_save
; ═══════════════════════════════════════════════════════════════════════════════

    [bits 64]
    section .text

    extern syscall_dispatch

    global syscall_entry

syscall_entry:
    ; ── Swap to kernel stack ──────────────────────────────────────────────
    ; Save user RSP to per-core data [gs:16]
    mov [gs:16], rsp

    ; Load kernel stack from per-core data [gs:8]
    mov rsp, [gs:8]

    ; ── Save user-mode registers ──────────────────────────────────────────
    push rcx            ; user RIP (return address)
    push r11            ; user RFLAGS
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; ── Call C dispatcher ─────────────────────────────────────────────────
    ; RAX = syscall_nr, RDI = arg1, RSI = arg2, RDX = arg3
    ; These are already in the right registers per SysV calling convention,
    ; but we need to move syscall_nr from RAX to RDI and shift args.
    mov r12, rdi        ; save arg1
    mov r13, rsi        ; save arg2
    mov r14, rdx        ; save arg3

    mov rdi, rax        ; arg0 = syscall_nr
    mov rsi, r12        ; arg1
    mov rdx, r13        ; arg2
    mov rcx, r14        ; arg3
    call syscall_dispatch
    ; Return value is in RAX

    ; ── Restore user-mode registers ───────────────────────────────────────
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    pop r11             ; user RFLAGS
    pop rcx             ; user RIP

    ; ── Swap back to user stack ───────────────────────────────────────────
    mov rsp, [gs:16]

    ; ── Return to user mode ───────────────────────────────────────────────
    ; SYSRETQ: RIP ← RCX, RFLAGS ← R11
    ;   CS = STAR[63:48] + 16 = 0x10 + 16 = 0x20 | RPL3 (user code)  ✓
    ;   SS = STAR[63:48] +  8 = 0x10 +  8 = 0x18 | RPL3 (user data)  ✓
    o64 sysret
