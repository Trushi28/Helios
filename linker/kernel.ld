/* ═══════════════════════════════════════════════════════════════════════════ */
/* Helios Kernel Linker Script                                                */
/* Maps kernel to upper canonical half of the SASOS 64-bit address space.     */
/* ═══════════════════════════════════════════════════════════════════════════ */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

KERNEL_VMA = 0xFFFFFFFFFF000000;

SECTIONS {
    . = KERNEL_VMA;

    /* ── Executable code ─────────────────────────────────────────────────── */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        KEEP(*(.text.entry))     /* kernel_entry must be first */
        *(.text .text.*)
    }

    /* ── Read-only data ──────────────────────────────────────────────────── */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    /* ── Initialized read-write data ─────────────────────────────────────── */
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    /* ── Uninitialized data (BSS) ────────────────────────────────────────── */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    /* ── Kernel end marker ───────────────────────────────────────────────── */
    __kernel_end = .;

    /* ── Discard unneeded sections ────────────────────────────────────────── */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame*)
        *(.gnu.hash)
        *(.gnu.version*)
    }
}
