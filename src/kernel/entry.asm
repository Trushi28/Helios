	; ═══════════════════════════════════════════════════════════════════════════════
	; Helios Kernel Entry Point
	; ═══════════════════════════════════════════════════════════════════════════════

	; Called by the UEFI bootloader after ExitBootServices().
	; CPU state on entry:
	;   RCX = physical address of boot_info_t  (Microsoft x64 ABI — UEFI
	;          calling convention passes the first argument in RCX, NOT RDI)
	;   RSI = 0 (reserved)
	;   Mode: 64-bit long mode, paging active (UEFI identity map)
	;   Interrupts: disabled (CLI)

	; The kernel is linked at KERNEL_VMA (high canonical half, see kernel.ld) but
	; is loaded by the bootloader as a flat binary at an arbitrary LOW physical
	; address. UEFI's page tables only map physical/identity memory — there is
	; no mapping for KERNEL_VMA. Because the kernel is compiled with
	; -mcmodel=kernel -fno-pic -fno-pie, any C code that takes the address of a
	; global (gdt_install's &g_gdt_ptr / &g_gdt, idt_install's &g_idt, etc.)
	; materializes that address as the link-time HIGH VMA, not a RIP-relative
	; runtime address. The very first such access (gdt_install's `lgdt`) faults
	; with #PF because that high VMA has no page-table entry yet.

	; This stub therefore, in order:
	; 1. Stashes boot_info_t* in r15 (callee-saved — survives the stack
	; switch below; the previous push/pop pair straddled two different
	; stacks and silently corrupted the pointer).
	; 2. Builds temporary bootstrap page tables in .bss (early_paging_setup):
	; - Identity-maps physical 0-4 GiB with 2 MiB pages, so the
	; current low-physical RIP keeps executing correctly.
	; - Maps KERNEL_VMA -> kernel_phys (PML4[511]/PDPT[511]/PD[504]
	; see 02-MEMORY.md) using 4 KiB pages. UEFI's AllocatePages()
	; only guarantees 4 KiB alignment, so a 2 MiB PDE here would
	; have non-zero low bits where the CPU requires them to be
	; reserved-zero -> #PF reserved-bit violation.
	; 3. Loads CR3 with the new PML4.
	; 4. Switches to the high-half kernel stack.
	; 5. Installs GDT, IDT (now safe — the high VMA they touch is mapped).
	; 6. Calls kernel_main(boot_info_t *boot_info).

	; NOTE: This bootstrap mapping is intentionally coarse (present+writable
	; only, no W^X split, no NX) and only needs to cover what Phase 0 touches.
	; The real per-region SASOS mapping is built later by vmm_init_sasos()
	; (Phase 1 — see 02-MEMORY.md, Section 3/4).
	; ═══════════════════════════════════════════════════════════════════════════════

	[bits   64]
	section .text.entry

	global kernel_entry
	extern kernel_main
	extern gdt_install
	extern idt_install

	; ── Bootstrap paging constants ──────────────────────────────────────────────
	PTE_PRESENT      equ (1 << 0)
	PTE_WRITABLE     equ (1 << 1)
	PTE_HUGE_PAGE    equ (1 << 7)
	PTE_GLOBAL       equ (1 << 8)

	; PML4/PDPT/PD indices for KERNEL_VMA = 0xFFFFFFFFFF000000.
	; Verified: PML4_INDEX=511, PDPT_INDEX=511, PD_INDEX=504, PT_INDEX=0.
	KVMA_PML4_IDX    equ 511
	KVMA_PDPT_IDX    equ 511
	KVMA_PD_IDX      equ 504

kernel_entry:
	cli

	;   ── Stash boot_info pointer in a callee-saved register ──────────────
	;   r15 is preserved across the C calls below (SysV ABI) and survives
	;   the RSP switch, unlike push-before/pop-after-switch which reads
	;   back from a different (uninitialized) stack.
	mov r15, rcx

	;    ── Build bootstrap page tables and load CR3 ─────────────────────────
	call early_paging_setup

	;   ── Set up the high-half kernel stack ────────────────────────────────
	lea rsp, [rel kernel_stack_top]

	;    ── Install GDT and IDT ──────────────────────────────────────────────
	call gdt_install
	call idt_install

	;    ── Call kernel_main(boot_info_t *boot_info) ─────────────────────────
	mov  rdi, r15
	xor  rbp, rbp
	call kernel_main

	; ── Should never return, but halt if it does ─────────────────────────

.hang:
	cli
	hlt
	jmp .hang

	; ═══════════════════════════════════════════════════════════════════════════════
	; early_paging_setup
	; ═══════════════════════════════════════════════════════════════════════════════
	; Builds a temporary PML4 that:
	; - PML4[0]   -> boot_pdpt_low  : identity-maps phys 0-4 GiB, 2 MiB pages
	; - PML4[511] -> boot_pdpt_high -> boot_pd_high[504] -> boot_pt_high :
	; maps KERNEL_VMA..KERNEL_VMA+2MiB -> kernel_phys, 4 KiB pages
	; Loads CR3 with the new PML4. Clobbers rax-rdx, rsi, r8, rdi, rcx.
	; Must run before any access to a high-VMA C global (GDT/IDT install).
	; ═══════════════════════════════════════════════════════════════════════════════

early_paging_setup:
	;   ── Zero all bootstrap table memory in one pass ──────────────────────
	;   boot_pml4, boot_pdpt_low, boot_pd_low(x4), boot_pdpt_high, boot_pd_high
	;   and boot_pt_high are laid out contiguously in .bss (9 pages, in that
	;   exact order — see the .bss block below). Zeroing them as one block
	;   keeps this routine simple and avoids a per-table helper call.
	cld
	lea rdi, [rel boot_pml4]
	xor eax, eax
	mov rcx, 9*512; 9 pages * 512 qwords/page
	rep stosq

	;   kernel_phys = physical (runtime) address of kernel_entry. kernel_entry
	;   is the first thing in .text.entry (KEEP'd first in kernel.ld) which is
	;   linked at LMA 0, and the bootloader loads the flat binary starting at
	;   kernel_phys — so the RIP-relative address of this label IS kernel_phys.
	lea rsi, [rel kernel_entry]

	;   ── PML4[0] -> boot_pdpt_low ──────────────────────────────────────────
	lea rax, [rel boot_pml4]
	lea rbx, [rel boot_pdpt_low]
	or  rbx, PTE_PRESENT | PTE_WRITABLE
	mov [rax], rbx

	;   ── PML4[511] -> boot_pdpt_high ───────────────────────────────────────
	lea rbx, [rel boot_pdpt_high]
	or  rbx, PTE_PRESENT | PTE_WRITABLE
	mov [rax + KVMA_PML4_IDX*8], rbx

	;   ── boot_pdpt_low[0..3] -> boot_pd_low[0..3] (4 x 1 GiB via 2 MiB PDEs) ─
	lea rax, [rel boot_pdpt_low]
	lea rcx, [rel boot_pd_low]
	xor rdx, rdx

.pdpt_low_loop:
	mov rbx, rcx
	or  rbx, PTE_PRESENT | PTE_WRITABLE
	mov [rax + rdx*8], rbx
	add rcx, 4096
	inc rdx
	cmp rdx, 4
	jl  .pdpt_low_loop

	;   ── Fill boot_pd_low: 4 tables x 512 entries x 2 MiB pages = 4 GiB ────
	lea rcx, [rel boot_pd_low]; rcx = current PD table base
	xor r8, r8; r8  = running identity phys address
	xor rdx, rdx; rdx = which of the 4 PD tables

.pd_low_table_loop:
	xor rbx, rbx; rbx = PD entry index 0..511

.pd_low_entry_loop:
	mov rax, r8
	or  rax, PTE_PRESENT | PTE_WRITABLE | PTE_HUGE_PAGE | PTE_GLOBAL
	mov [rcx + rbx*8], rax
	add r8, 0x200000; next 2 MiB
	inc rbx
	cmp rbx, 512
	jl  .pd_low_entry_loop
	add rcx, 4096
	inc rdx
	cmp rdx, 4
	jl  .pd_low_table_loop

	;   ── boot_pdpt_high[511] -> boot_pd_high ───────────────────────────────
	lea rax, [rel boot_pdpt_high]
	lea rbx, [rel boot_pd_high]
	or  rbx, PTE_PRESENT | PTE_WRITABLE
	mov [rax + KVMA_PDPT_IDX*8], rbx

	;   ── boot_pd_high[504] -> boot_pt_high ─────────────────────────────────
	lea rax, [rel boot_pd_high]
	lea rbx, [rel boot_pt_high]
	or  rbx, PTE_PRESENT | PTE_WRITABLE
	mov [rax + KVMA_PD_IDX*8], rbx

	;   ── boot_pt_high[0..511] -> kernel_phys + i*4 KiB (4 KiB pages) ───────
	lea rax, [rel boot_pt_high]
	xor rbx, rbx

.pt_high_loop:
	mov rdx, rsi
	or  rdx, PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL
	mov [rax + rbx*8], rdx
	add rsi, 4096
	inc rbx
	cmp rbx, 512
	jl  .pt_high_loop

	;   ── Activate the new page tables ──────────────────────────────────────
	lea rax, [rel boot_pml4]
	mov cr3, rax
	ret

	;       ═══════════════════════════════════════════════════════════════════════════════
	;       Kernel Stack (64 KiB) and bootstrap page tables, all in BSS
	;       ═══════════════════════════════════════════════════════════════════════════════
	section .bss
	align   16

kernel_stack_bottom:
	resb 65536; 64 KiB stack

kernel_stack_top:

	;     The six bootstrap-table labels below MUST stay contiguous and in this
	;     exact order (early_paging_setup zeroes all 9 pages as a single block).
	align 4096
	boot_pml4:        resb 4096
	boot_pdpt_low:    resb 4096
	boot_pd_low:      resb 4096*4
	boot_pdpt_high:   resb 4096
	boot_pd_high:     resb 4096
	boot_pt_high:     resb 4096
