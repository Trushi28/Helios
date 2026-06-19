/**
 * @file boot_info.h
 * @brief Boot information structure passed from UEFI bootloader to kernel.
 *
 * This is the ONLY data channel between the bootloader and the kernel.
 * The bootloader populates this structure and passes its physical address
 * to kernel_entry in RDI.
 */

#ifndef HELIOS_BOOT_INFO_H
#define HELIOS_BOOT_INFO_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Boot protocol magic                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define BOOT_INFO_MAGIC     0x48454C494F53424FULL  /* "HELIOSBO" */
#define BOOT_INFO_VERSION   1

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Memory map types (Helios-native, translated from UEFI types)              */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    HELIOS_MEM_FREE           = 0,  /* Usable conventional memory            */
    HELIOS_MEM_BOOTLOADER     = 1,  /* Bootloader code/data (reclaimable)    */
    HELIOS_MEM_KERNEL         = 2,  /* Kernel image                          */
    HELIOS_MEM_UEFI_RUNTIME   = 3,  /* UEFI runtime services (preserve)     */
    HELIOS_MEM_ACPI_RECLAIM   = 4,  /* ACPI tables (reclaimable after parse) */
    HELIOS_MEM_ACPI_NVS       = 5,  /* ACPI non-volatile storage (preserve) */
    HELIOS_MEM_MMIO           = 6,  /* Memory-mapped I/O                     */
    HELIOS_MEM_RESERVED       = 7,  /* Reserved, do not use                  */
    HELIOS_MEM_UNUSABLE       = 8,  /* Unusable / defective                  */
    HELIOS_MEM_FRAMEBUFFER    = 9,  /* GOP framebuffer                       */
    HELIOS_MEM_BOOT_INFO      = 10, /* boot_info_t structure itself          */
} helios_mem_type_t;

/**
 * @brief Single memory map entry (Helios-native format).
 */
typedef struct {
    uint64_t phys_base;     /**< Physical base address (page-aligned)   */
    uint64_t page_count;    /**< Number of 4 KiB pages                  */
    uint32_t type;          /**< helios_mem_type_t enum value           */
    uint32_t attributes;    /**< Cacheability: WB/WC/UC etc.            */
} PACKED helios_mem_entry_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Boot info structure                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* ── Magic & Version ─────────────────────────────────────────────────── */
    uint64_t magic;              /**< BOOT_INFO_MAGIC                       */
    uint32_t version;            /**< Boot protocol version (1)             */
    uint32_t _reserved0;

    /* ── Framebuffer ─────────────────────────────────────────────────────── */
    struct {
        uint64_t base_phys;      /**< Physical address of linear framebuffer */
        uint32_t width;          /**< Horizontal resolution in pixels        */
        uint32_t height;         /**< Vertical resolution in pixels          */
        uint32_t pitch;          /**< Bytes per scanline                     */
        uint32_t bpp;            /**< Bits per pixel (expect 32)             */
        uint32_t red_mask_size;
        uint32_t red_mask_shift;
        uint32_t green_mask_size;
        uint32_t green_mask_shift;
        uint32_t blue_mask_size;
        uint32_t blue_mask_shift;
        uint32_t _pad;
    } framebuffer;

    /* ── Memory Map ──────────────────────────────────────────────────────── */
    struct {
        uint64_t entries_phys;   /**< Physical address of helios_mem_entry_t[] */
        uint64_t entry_count;    /**< Number of entries                       */
        uint64_t entry_size;     /**< Size of each entry in bytes             */
    } memory_map;

    /* ── ACPI ────────────────────────────────────────────────────────────── */
    struct {
        uint64_t rsdp_phys;      /**< Physical address of ACPI RSDP (v2.0+) */
    } acpi;

    /* ── SMBIOS ──────────────────────────────────────────────────────────── */
    struct {
        uint64_t smbios3_phys;   /**< Physical address of SMBIOS 3.0 entry  */
    } smbios;

    /* ── Kernel Image ────────────────────────────────────────────────────── */
    struct {
        uint64_t phys_base;      /**< Where kernel was loaded in phys memory */
        uint64_t size;           /**< Size of kernel image in bytes          */
        uint64_t entry_offset;   /**< Offset to kernel_entry from phys_base  */
    } kernel;

    /* ── Boot Timestamps ─────────────────────────────────────────────────── */
    uint64_t tsc_at_exit_boot;   /**< TSC value at ExitBootServices()        */
} PACKED boot_info_t;

#endif /* HELIOS_BOOT_INFO_H */
