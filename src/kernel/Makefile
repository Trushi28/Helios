# ═══════════════════════════════════════════════════════════════════════════════
# Helios Kernel Makefile
# ═══════════════════════════════════════════════════════════════════════════════
# Builds the kernel as a flat binary (kernel.bin) from:
#   - C sources (kernel core + arch-specific)
#   - NASM assembly (entry, IDT stubs)
# ═══════════════════════════════════════════════════════════════════════════════

include ../../config.mk

# ── Source discovery ─────────────────────────────────────────────────────────
C_SRC := main.c serial.c string.c printf.c panic.c \
         arch/x86_64/gdt.c arch/x86_64/idt.c

ASM_SRC := entry.asm arch/x86_64/idt_stubs.asm

C_OBJ   := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(C_SRC))
ASM_OBJ := $(patsubst %.asm,$(KERNEL_BUILD)/%.o,$(ASM_SRC))

KERNEL_ELF := $(KERNEL_BUILD)/kernel.elf
KERNEL_BIN := $(KERNEL_BUILD)/kernel.bin

.PHONY: all clean

all: $(KERNEL_BIN)

# ── Create build directories ────────────────────────────────────────────────
$(KERNEL_BUILD):
	mkdir -p $(KERNEL_BUILD)
	mkdir -p $(KERNEL_BUILD)/arch/x86_64

# ── Compile C sources ───────────────────────────────────────────────────────
$(KERNEL_BUILD)/%.o: %.c | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

# ── Assemble NASM sources ───────────────────────────────────────────────────
$(KERNEL_BUILD)/%.o: %.asm | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# ── Link kernel ELF ─────────────────────────────────────────────────────────
$(KERNEL_ELF): $(ASM_OBJ) $(C_OBJ)
	$(LD) $(LDFLAGS_KERNEL) -o $@ $^

# ── Convert to flat binary ──────────────────────────────────────────────────
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(KERNEL_BUILD)
