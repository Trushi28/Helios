# ═══════════════════════════════════════════════════════════════════════════════
# Helios OS — Build Configuration
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Toolchain ───────────────────────────────────────────────────────────────
CC          := x86_64-elf-gcc
AS          := nasm
LD          := x86_64-elf-ld
OBJCOPY     := x86_64-elf-objcopy
AR          := x86_64-elf-ar

# ─── Directories ─────────────────────────────────────────────────────────────
ROOT_DIR    := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
SRC_DIR     := $(ROOT_DIR)/src
BUILD_DIR   := $(ROOT_DIR)/build
INCLUDE_DIR := $(SRC_DIR)/include
LINKER_DIR  := $(ROOT_DIR)/linker
TOOLS_DIR   := $(ROOT_DIR)/tools

# ─── Common Compiler Flags ──────────────────────────────────────────────────
CFLAGS_COMMON := -std=c2x -Wall -Wextra -Werror \
                 -fno-exceptions -fno-unwind-tables \
                 -fstack-protector-strong \
                 -I$(INCLUDE_DIR)

# ─── Kernel Compiler Flags ──────────────────────────────────────────────────
CFLAGS_KERNEL := $(CFLAGS_COMMON) \
                 -ffreestanding -nostdlib -nostdinc \
                 -mcmodel=kernel -mno-red-zone \
                 -mno-mmx -mno-sse -mno-sse2 \
                 -fno-pic -fno-pie \
                 -DHELIOS_KERNEL

# ─── Bootloader Compiler Flags ──────────────────────────────────────────────
# The bootloader is built as a PE32+ UEFI application.
# We use the GNU-EFI headers / approach: compile as ELF, then objcopy to PE32+.
CFLAGS_BOOT   := $(CFLAGS_COMMON) \
                 -ffreestanding -nostdlib \
                 -fno-pic \
                 -mno-red-zone \
                 -fno-stack-protector \
                 -DHELIOS_BOOT

# ─── User Micro-Program Compiler Flags (future) ────────────────────────────
CFLAGS_USER   := $(CFLAGS_COMMON) \
                 -ffreestanding -nostdlib \
                 -fpic -fPIE \
                 -DHELIOS_USER

# ─── Assembler Flags ────────────────────────────────────────────────────────
ASFLAGS     := -f elf64 -g -F dwarf

# ─── Linker Flags ───────────────────────────────────────────────────────────
LDFLAGS_KERNEL := -T $(LINKER_DIR)/kernel.ld -nostdlib -static -z noexecstack
LDFLAGS_BOOT   := -T $(LINKER_DIR)/bootloader.ld -nostdlib -static \
                  -z noexecstack

# ─── Debug / Release ────────────────────────────────────────────────────────
DEBUG       ?= 1
ifeq ($(DEBUG),1)
    CFLAGS_COMMON += -g3 -O0 -DHELIOS_DEBUG
else
    CFLAGS_COMMON += -O2 -DNDEBUG
endif

# ─── Output Paths ───────────────────────────────────────────────────────────
BOOT_BUILD    := $(BUILD_DIR)/boot
KERNEL_BUILD  := $(BUILD_DIR)/kernel
IMAGE         := $(BUILD_DIR)/helios.img
ESP_DIR       := $(BUILD_DIR)/esp
