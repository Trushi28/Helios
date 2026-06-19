# ═══════════════════════════════════════════════════════════════════════════════
# Helios Bootloader Makefile
# ═══════════════════════════════════════════════════════════════════════════════
# Builds the UEFI bootloader as a PE32+ application (BOOTX64.EFI).
#
# The bootloader is compiled with clang targeting x86_64-unknown-windows
# (produces COFF objects) and linked with lld in COFF mode to produce a
# native PE32+ UEFI executable. The kernel continues to use x86_64-elf-gcc.
# ═══════════════════════════════════════════════════════════════════════════════

include ../../config.mk

# Override compiler for the bootloader — clang targeting Windows/UEFI (COFF ABI)
BOOT_CC     := clang
BOOT_CFLAGS := -target x86_64-unknown-windows \
               -std=c2x -Wall -Wextra -Werror \
               -ffreestanding -nostdlib \
               -fno-exceptions -fno-unwind-tables \
               -fno-stack-protector \
               -fno-pic \
               -mno-red-zone \
               -fshort-wchar \
               -I$(INCLUDE_DIR) \
               -DHELIOS_BOOT

ifeq ($(DEBUG),1)
    BOOT_CFLAGS += -g -O0 -DHELIOS_DEBUG
else
    BOOT_CFLAGS += -O2 -DNDEBUG
endif

BOOT_SRC    := $(wildcard *.c)
BOOT_OBJ    := $(patsubst %.c,$(BOOT_BUILD)/%.o,$(BOOT_SRC))
BOOT_EFI    := $(BOOT_BUILD)/BOOTX64.EFI

# Bootloader needs string functions too
BOOT_STRING_OBJ := $(BOOT_BUILD)/string.o

.PHONY: all clean

all: $(BOOT_EFI)

$(BOOT_BUILD):
	mkdir -p $(BOOT_BUILD)

# Compile bootloader C sources (produces COFF .obj files)
$(BOOT_BUILD)/%.o: %.c | $(BOOT_BUILD)
	$(BOOT_CC) $(BOOT_CFLAGS) -c $< -o $@

# Compile string.c for the bootloader
$(BOOT_STRING_OBJ): ../kernel/string.c | $(BOOT_BUILD)
	$(BOOT_CC) $(BOOT_CFLAGS) -c $< -o $@

# Link into PE32+ UEFI application using lld in COFF mode
$(BOOT_EFI): $(BOOT_OBJ) $(BOOT_STRING_OBJ)
	lld -flavor link \
		/subsystem:efi_application \
		/entry:efi_main \
		/out:$@ \
		$^

clean:
	rm -rf $(BOOT_BUILD)
