# ═══════════════════════════════════════════════════════════════════════════════
# Helios OS — Top-Level Makefile
# ═══════════════════════════════════════════════════════════════════════════════

include config.mk

.PHONY: all boot kernel image run debug clean help

# ── Default target ───────────────────────────────────────────────────────────
all: image

# ── Build bootloader ─────────────────────────────────────────────────────────
boot:
	$(MAKE) -C src/boot

# ── Build kernel ─────────────────────────────────────────────────────────────
kernel:
	$(MAKE) -C src/kernel

# ── Build disk image ─────────────────────────────────────────────────────────
image: boot kernel
	python3 tools/mkimage.py \
		--bootloader $(BOOT_BUILD)/BOOTX64.EFI \
		--kernel $(KERNEL_BUILD)/kernel.bin \
		--output $(IMAGE)
	@echo ""
	@echo "  ✓ Disk image: $(IMAGE)"
	@echo "  Run with:     make run"
	@echo ""

# ── Run in QEMU ──────────────────────────────────────────────────────────────
run: image
	bash tools/qemu-run.sh $(IMAGE)

# ── Run with GDB debug stub ─────────────────────────────────────────────────
debug: image
	bash tools/qemu-run.sh $(IMAGE) --debug

# ── Clean all build artifacts ────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

# ── Help ─────────────────────────────────────────────────────────────────────
help:
	@echo "Helios OS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       Build everything (default)"
	@echo "  boot      Build UEFI bootloader"
	@echo "  kernel    Build kernel"
	@echo "  image     Build disk image (GPT + ESP)"
	@echo "  run       Build and run in QEMU"
	@echo "  debug     Build and run with GDB stub"
	@echo "  clean     Remove all build artifacts"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1   Debug build (default)"
	@echo "  DEBUG=0   Release build (optimized)"
