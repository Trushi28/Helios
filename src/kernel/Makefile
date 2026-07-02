# ═══════════════════════════════════════════════════════════════════════════════
# Helios Kernel Makefile
# ═══════════════════════════════════════════════════════════════════════════════
# Builds the kernel as a flat binary (kernel.bin) from:
#   - C sources (kernel core + arch-specific + scheduler + syscall + test)
#   - NASM assembly (entry, IDT stubs, context switch, syscall entry)
#   - AP trampoline binary blob (assembled as flat binary, linked via objcopy)
# ═══════════════════════════════════════════════════════════════════════════════

include ../../config.mk

# ── Source discovery ─────────────────────────────────────────────────────────
C_SRC := main.c serial.c string.c printf.c panic.c \
         arch/x86_64/gdt.c arch/x86_64/idt.c \
         arch/x86_64/tsc.c arch/x86_64/x2apic.c \
         mm/pmm.c mm/vmm.c mm/slab.c mm/capability.c \
         crypto/sha256.c crypto/hmac.c crypto/random.c \
         acpi/madt.c \
         sched/per_core.c sched/microprog.c sched/scheduler.c sched/smp.c \
         syscall/syscall.c \
         test/phase2_test.c

ASM_SRC := entry.asm arch/x86_64/idt_stubs.asm \
           sched/context.asm syscall/syscall_entry.asm

C_OBJ   := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(C_SRC))
ASM_OBJ := $(patsubst %.asm,$(KERNEL_BUILD)/%.o,$(ASM_SRC))

# AP trampoline — assembled as flat binary, then converted to linkable object
AP_TRAMP_ASM := arch/x86_64/ap_trampoline.asm
AP_TRAMP_BIN := $(KERNEL_BUILD)/arch/x86_64/ap_trampoline.bin
AP_TRAMP_OBJ := $(KERNEL_BUILD)/arch/x86_64/ap_trampoline_blob.o

KERNEL_ELF := $(KERNEL_BUILD)/kernel.elf
KERNEL_BIN := $(KERNEL_BUILD)/kernel.bin

.PHONY: all clean

all: $(KERNEL_BIN)

# ── Create build directories ────────────────────────────────────────────────
$(KERNEL_BUILD):
	mkdir -p $(KERNEL_BUILD)
	mkdir -p $(KERNEL_BUILD)/arch/x86_64
	mkdir -p $(KERNEL_BUILD)/mm
	mkdir -p $(KERNEL_BUILD)/crypto
	mkdir -p $(KERNEL_BUILD)/acpi
	mkdir -p $(KERNEL_BUILD)/sched
	mkdir -p $(KERNEL_BUILD)/syscall
	mkdir -p $(KERNEL_BUILD)/test

# ── Compile C sources ───────────────────────────────────────────────────────
$(KERNEL_BUILD)/%.o: %.c | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

# ── Assemble NASM sources (ELF64) ───────────────────────────────────────────
$(KERNEL_BUILD)/%.o: %.asm | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# ── AP trampoline: assemble as flat binary, then wrap with objcopy ───────────
$(AP_TRAMP_BIN): $(AP_TRAMP_ASM) | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	$(AS) -f bin $< -o $@

$(AP_TRAMP_OBJ): $(AP_TRAMP_BIN)
	cd $(dir $<) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$(notdir $<) $(abspath $@)

# ── Link kernel ELF ─────────────────────────────────────────────────────────
$(KERNEL_ELF): $(ASM_OBJ) $(C_OBJ) $(AP_TRAMP_OBJ)
	$(LD) $(LDFLAGS_KERNEL) -o $@ $^

# ── Convert to flat binary ──────────────────────────────────────────────────
# --set-section-flags .bss=alloc,load,contents: Forces objcopy to emit
#   the .bss section as zero-filled data in the flat binary, so the file
#   size reflects the FULL kernel footprint (.text + .rodata + .data + .bss).
#   Without this, the bootloader's file_size-based allocation would only
#   cover initialized sections, leaving .bss (kernel stack, bootstrap page
#   tables) overlapping with memory that UEFI reports as EfiConventionalMemory.
# --gap-fill 0: Fills any inter-section alignment gaps with zeros.
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary \
		--set-section-flags .bss=alloc,load,contents \
		--gap-fill 0 \
		$< $@

clean:
	rm -rf $(KERNEL_BUILD)
