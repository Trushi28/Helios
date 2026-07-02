	; ═══════════════════════════════════════════════════════════════════════════════
	; Helios — AP Trampoline (16-bit real → 32-bit protected → 64-bit long mode)
	; ═══════════════════════════════════════════════════════════════════════════════

	%define AP_DATA_OFFSET 0x0F00

	[bits 16]
	[org  0x8000]

ap_start:
	cli
	cld

	;   1. Set up 16-bit segments
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax

	;    2. Load temporary PHYSICAL GDT for the 16 -> 32 transition
	lgdt [trampoline_gdtr]

	;   3. Enable protected mode (PE bit in CR0)
	mov eax, cr0
	or  eax, 1
	mov cr0, eax

	;   4. Far jump to 32-bit protected mode
	jmp dword 0x08:ap_pm32

[bits 32]

ap_pm32:
	;   5. Set data segments to temporary 32-bit Data Selector (0x10)
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax

	;   6. Enable PAE (CR4.PAE = bit 5)
	mov eax, cr4
	or  eax, (1 << 5)
	mov cr4, eax

	;   7. Load PML4 from data block
	mov eax, [0x8000 + AP_DATA_OFFSET + 0x00]
	mov cr3, eax

	;   8. Enable long mode (EFER.LME = bit 8) and NXE (bit 11)
	mov ecx, 0xC0000080
	rdmsr
	or  eax, (1 << 8) | (1 << 11)
	wrmsr

	;   9. Enable paging (CR0.PG = bit 31)
	mov eax, cr0
	or  eax, (1 << 31)
	mov cr0, eax

	;   10. We are now in Compatibility Mode.
	;   Jump to 64-bit mode using the Trampoline's 64-bit Code Selector (0x18)
	jmp dword 0x18:ap_lm64

[bits 64]

ap_lm64:
	;    11. WE ARE NOW IN 64-BIT LONG MODE!
	;    Because we are in 64-bit mode, the lgdt instruction will now correctly
	;    read the full 64-bit virtual base address provided by the BSP.
	lgdt [0x8000 + AP_DATA_OFFSET + 0x18]

	;   12. Set data segments using the Kernel's real Data Selector (0x10)
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	xor ax, ax
	mov fs, ax
	mov gs, ax

	;   13. Set up the per-AP stack
	mov rsp, [0x8000 + AP_DATA_OFFSET + 0x08]

	;    14. We must reload CS with the Kernel's real 64-bit Code Selector (0x08).
	;    We cannot use a normal jump, so we push the selector and RIP, then retfq.
	push 0x08
	mov  rax, .reload_cs
	push rax
	retfq

.reload_cs:
	;   15. Pass ap_index in RDI (first argument to ap_entry)
	xor rdi, rdi
	mov edi, [0x8000 + AP_DATA_OFFSET + 0x2C]

	;    16. Jump to the C entry point in the higher-half kernel
	mov  rax, [0x8000 + AP_DATA_OFFSET + 0x10]
	call rax

	; Should never return — halt if it does
	cli

.spin:
	hlt
	jmp .spin

	;     ═══════════════════════════════════════════════════════════════════════════════
	;     Temporary GDT for Trampoline Bootstrap
	;     ═══════════════════════════════════════════════════════════════════════════════
	align 16

trampoline_gdt:
	dq 0x0000000000000000; 0x00: Null descriptor
	dq 0x00CF9A000000FFFF; 0x08: 32-bit Code (RX)
	dq 0x00CF92000000FFFF; 0x10: 32-bit Data (RW)
	dq 0x00AF9A000000FFFF; 0x18: 64-bit Code (RX, L=1)

trampoline_gdt_end:

trampoline_gdtr:
	dw trampoline_gdt_end - trampoline_gdt - 1
	dd trampoline_gdt

ap_trampoline_end:
