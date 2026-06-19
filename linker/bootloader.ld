/* ═══════════════════════════════════════════════════════════════════════════ */
/* Helios UEFI Bootloader Linker Script                                       */
/* Produces an ELF that is later converted to PE32+ via objcopy.              */
/* ═══════════════════════════════════════════════════════════════════════════ */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS {
    . = 0;

    .text ALIGN(4K) : {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : {
        *(.bss .bss.*)
        *(COMMON)
    }

    .reloc ALIGN(4K) : {
        *(.reloc)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame*)
    }
}
