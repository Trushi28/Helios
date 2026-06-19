/**
 * @file bootx64.c
 * @brief Helios UEFI Bootloader — efi_main() entry point.
 *
 * This UEFI application:
 *   1. Locates and loads KERNEL.BIN from the EFI System Partition
 *   2. Queries the GOP for a framebuffer
 *   3. Retrieves the UEFI memory map
 *   4. Locates the ACPI RSDP and SMBIOS from the EFI Configuration Table
 *   5. Populates a boot_info_t structure
 *   6. Calls ExitBootServices()
 *   7. Jumps to kernel_entry with boot_info_t* in RDI
 *
 * No UEFI headers — we define the minimal structures inline to avoid
 * external dependencies.
 */

#include <helios/types.h>
#include <helios/boot_info.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  UEFI Type Definitions (minimal subset)                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef uint64_t UINTN;
typedef int64_t  INTN;
typedef uint64_t EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef uint16_t CHAR16;

/* UEFI GUIDs */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} EFI_GUID;

/* Status codes */
#define EFI_SUCCESS             0
#define EFI_BUFFER_TOO_SMALL    ((EFI_STATUS)(0x8000000000000000ULL | 5))

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  UEFI System Table (minimal)                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declarations for the table structures we need */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID   vendor_guid;
    void      *vendor_table;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    char                    _hdr[24];    /* EFI_TABLE_HEADER */
    CHAR16                 *firmware_vendor;
    uint32_t                firmware_revision;
    uint32_t                _pad0;
    EFI_HANDLE              console_in_handle;
    void                   *con_in;
    EFI_HANDLE              console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE              stderr_handle;
    void                   *std_err;
    EFI_RUNTIME_SERVICES   *runtime_services;
    EFI_BOOT_SERVICES      *boot_services;
    UINTN                   number_of_table_entries;
    EFI_CONFIGURATION_TABLE *configuration_table;
} EFI_SYSTEM_TABLE;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Console Output Protocol (for early debug)                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *_reset;
    EFI_STATUS (*output_string)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, CHAR16 *string);
    /* ... other fields not needed */
};

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Graphics Output Protocol (GOP)                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    PIXEL_RGB  = 0,
    PIXEL_BGR  = 1,
    PIXEL_BITMASK = 2,
    PIXEL_BLT_ONLY = 3,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t                    version;
    uint32_t                    horizontal_resolution;
    uint32_t                    vertical_resolution;
    EFI_GRAPHICS_PIXEL_FORMAT   pixel_format;
    EFI_PIXEL_BITMASK           pixel_info;
    uint32_t                    pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                                max_mode;
    uint32_t                                mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   *info;
    UINTN                                   size_of_info;
    uint64_t                                frame_buffer_base;
    UINTN                                   frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*query_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
                             uint32_t mode_number, UINTN *size_of_info,
                             EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
    EFI_STATUS (*set_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
                           uint32_t mode_number);
    void *_blt;   /* Not used */
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  File I/O Protocol (Simple File System + File)                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct EFI_FILE_PROTOCOL {
    uint64_t revision;
    EFI_STATUS (*open)(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **new_handle,
                       CHAR16 *filename, uint64_t open_mode, uint64_t attributes);
    EFI_STATUS (*close)(EFI_FILE_PROTOCOL *self);
    void *_delete;
    EFI_STATUS (*read)(EFI_FILE_PROTOCOL *self, UINTN *buffer_size, void *buffer);
    void *_write;
    void *_get_position;
    void *_set_position;
    EFI_STATUS (*get_info)(EFI_FILE_PROTOCOL *self, EFI_GUID *info_type,
                           UINTN *buffer_size, void *buffer);
    /* ... */
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t revision;
    EFI_STATUS (*open_volume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                              EFI_FILE_PROTOCOL **root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* File info GUID */
#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

typedef struct {
    uint64_t size;
    uint64_t file_size;
    uint64_t physical_size;
    /* ... timestamps and name follow */
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Boot Services (minimal subset)                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    ALLOCATE_ANY_PAGES   = 0,
    ALLOCATE_MAX_ADDRESS = 1,
    ALLOCATE_ADDRESS     = 2,
} EFI_ALLOCATE_TYPE;

typedef enum {
    EFI_RESERVED_MEMORY_TYPE     = 0,
    EFI_LOADER_CODE              = 1,
    EFI_LOADER_DATA              = 2,
    EFI_BOOT_SERVICES_CODE       = 3,
    EFI_BOOT_SERVICES_DATA       = 4,
    EFI_RUNTIME_SERVICES_CODE    = 5,
    EFI_RUNTIME_SERVICES_DATA    = 6,
    EFI_CONVENTIONAL_MEMORY      = 7,
    EFI_UNUSABLE_MEMORY          = 8,
    EFI_ACPI_RECLAIM_MEMORY      = 9,
    EFI_ACPI_MEMORY_NVS          = 10,
    EFI_MEMORY_MAPPED_IO         = 11,
    EFI_MEMORY_MAPPED_IO_PORT    = 12,
    EFI_PAL_CODE                 = 13,
    EFI_PERSISTENT_MEMORY        = 14,
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t type;
    uint32_t _pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

struct EFI_BOOT_SERVICES {
    char _hdr[24];  /* EFI_TABLE_HEADER */

    /* Task Priority Services */
    void *_raise_tpl;
    void *_restore_tpl;

    /* Memory Services */
    EFI_STATUS (*allocate_pages)(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE memory_type,
                                 UINTN pages, uint64_t *memory);
    EFI_STATUS (*free_pages)(uint64_t memory, UINTN pages);
    EFI_STATUS (*get_memory_map)(UINTN *memory_map_size, EFI_MEMORY_DESCRIPTOR *memory_map,
                                 UINTN *map_key, UINTN *descriptor_size,
                                 uint32_t *descriptor_version);
    EFI_STATUS (*allocate_pool)(EFI_MEMORY_TYPE pool_type, UINTN size, void **buffer);
    EFI_STATUS (*free_pool)(void *buffer);

    /* Event & Timer Services */
    void *_create_event;
    void *_set_timer;
    void *_wait_for_event;
    void *_signal_event;
    void *_close_event;
    void *_check_event;

    /* Protocol Handler Services */
    void *_install_protocol;
    void *_reinstall_protocol;
    void *_uninstall_protocol;
    EFI_STATUS (*handle_protocol)(EFI_HANDLE handle, EFI_GUID *protocol, void **interface);
    void *_reserved;
    void *_register_protocol_notify;
    EFI_STATUS (*locate_handle)(uint32_t search_type, EFI_GUID *protocol,
                                 void *search_key, UINTN *buffer_size, EFI_HANDLE *buffer);
    void *_locate_device_path;
    void *_install_configuration_table;

    /* Image Services */
    void *_load_image;
    void *_start_image;
    void *_exit;
    void *_unload_image;
    EFI_STATUS (*exit_boot_services)(EFI_HANDLE image_handle, UINTN map_key);

    /* Misc Services */
    void *_get_next_monotonic_count;
    void *_stall;
    void *_set_watchdog_timer;

    /* DriverSupport Services */
    void *_connect_controller;
    void *_disconnect_controller;

    /* Open/Close Protocol */
    void *_open_protocol;
    void *_close_protocol;
    void *_open_protocol_information;

    /* Library Services */
    void *_protocols_per_handle;
    void *_locate_handle_buffer;
    EFI_STATUS (*locate_protocol)(EFI_GUID *protocol, void *registration, void **interface);
    /* ... more fields follow but are not needed */
};

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Well-known GUIDs                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

static EFI_GUID GOP_GUID = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

static EFI_GUID SFS_GUID = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

static EFI_GUID ACPI20_GUID = {
    0x8868e871, 0xe4f1, 0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};

static EFI_GUID SMBIOS3_GUID = {
    0xf2fd1544, 0x9794, 0x4a2c,
    {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}
};

static EFI_GUID FILE_INFO_GUID = EFI_FILE_INFO_GUID;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Helper: print to UEFI console                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

static EFI_SYSTEM_TABLE *g_st;

static void efi_print(const char *msg) {
    CHAR16 buf[256];
    int i = 0;
    while (*msg && i < 254) {
        if (*msg == '\n') {
            buf[i++] = L'\r';
        }
        buf[i++] = (CHAR16)*msg++;
    }
    buf[i] = 0;
    g_st->con_out->output_string(g_st->con_out, buf);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Helper: GUID comparison                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

extern int memcmp(const void *a, const void *b, size_t n);

static int guid_eq(EFI_GUID *a, EFI_GUID *b) {
    return memcmp(a, b, sizeof(EFI_GUID)) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Helper: translate UEFI memory type → Helios memory type                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t translate_mem_type(uint32_t uefi_type) {
    switch (uefi_type) {
    case EFI_CONVENTIONAL_MEMORY:    return HELIOS_MEM_FREE;
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:     return HELIOS_MEM_BOOTLOADER;
    case EFI_RUNTIME_SERVICES_CODE:
    case EFI_RUNTIME_SERVICES_DATA:  return HELIOS_MEM_UEFI_RUNTIME;
    case EFI_ACPI_RECLAIM_MEMORY:    return HELIOS_MEM_ACPI_RECLAIM;
    case EFI_ACPI_MEMORY_NVS:       return HELIOS_MEM_ACPI_NVS;
    case EFI_MEMORY_MAPPED_IO:
    case EFI_MEMORY_MAPPED_IO_PORT:  return HELIOS_MEM_MMIO;
    case EFI_UNUSABLE_MEMORY:        return HELIOS_MEM_UNUSABLE;
    case EFI_PERSISTENT_MEMORY:      return HELIOS_MEM_FREE;
    default:                         return HELIOS_MEM_RESERVED;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Minimal memcpy/memset (bootloader needs its own copy)                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *dest, int val, size_t n);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  efi_main — UEFI Application Entry Point                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    g_st = system_table;
    EFI_BOOT_SERVICES *bs = system_table->boot_services;
    EFI_STATUS status;

    efi_print("Helios Bootloader v0.1\n");

    /* ── 1. Allocate boot_info_t ─────────────────────────────────────────── */
    uint64_t boot_info_phys = 0;
    status = bs->allocate_pages(ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                 1, &boot_info_phys);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Failed to allocate boot_info page\n");
        return status;
    }
    boot_info_t *boot_info = (boot_info_t *)boot_info_phys;
    memset(boot_info, 0, sizeof(boot_info_t));
    boot_info->magic   = BOOT_INFO_MAGIC;
    boot_info->version = BOOT_INFO_VERSION;

    /* ── 2. Query GOP (framebuffer) ──────────────────────────────────────── */
    efi_print("  Querying GOP...\n");
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    status = bs->locate_protocol(&GOP_GUID, NULL, (void **)&gop);
    if (status == EFI_SUCCESS && gop && gop->mode && gop->mode->info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->mode->info;

        boot_info->framebuffer.base_phys = gop->mode->frame_buffer_base;
        boot_info->framebuffer.width     = mi->horizontal_resolution;
        boot_info->framebuffer.height    = mi->vertical_resolution;
        boot_info->framebuffer.pitch     = mi->pixels_per_scan_line * 4;
        boot_info->framebuffer.bpp       = 32;

        /* Determine pixel format */
        if (mi->pixel_format == PIXEL_RGB) {
            boot_info->framebuffer.red_mask_size   = 8;
            boot_info->framebuffer.red_mask_shift   = 0;
            boot_info->framebuffer.green_mask_size = 8;
            boot_info->framebuffer.green_mask_shift = 8;
            boot_info->framebuffer.blue_mask_size  = 8;
            boot_info->framebuffer.blue_mask_shift  = 16;
        } else if (mi->pixel_format == PIXEL_BGR) {
            boot_info->framebuffer.red_mask_size   = 8;
            boot_info->framebuffer.red_mask_shift   = 16;
            boot_info->framebuffer.green_mask_size = 8;
            boot_info->framebuffer.green_mask_shift = 8;
            boot_info->framebuffer.blue_mask_size  = 8;
            boot_info->framebuffer.blue_mask_shift  = 0;
        } else if (mi->pixel_format == PIXEL_BITMASK) {
            /* Parse bitmask — count bits and shift for each component */
            /* Simplified: assume 8 bits per component */
            boot_info->framebuffer.red_mask_size   = 8;
            boot_info->framebuffer.green_mask_size = 8;
            boot_info->framebuffer.blue_mask_size  = 8;
        }

        efi_print("  GOP: OK\n");
    } else {
        efi_print("  GOP: Not available (serial-only mode)\n");
    }

    /* ── 3. Scan Configuration Table for ACPI and SMBIOS ─────────────────── */
    efi_print("  Scanning config tables...\n");
    for (UINTN i = 0; i < system_table->number_of_table_entries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &system_table->configuration_table[i];
        if (guid_eq(&ct->vendor_guid, &ACPI20_GUID)) {
            boot_info->acpi.rsdp_phys = (uint64_t)ct->vendor_table;
            efi_print("  ACPI 2.0 RSDP: found\n");
        }
        if (guid_eq(&ct->vendor_guid, &SMBIOS3_GUID)) {
            boot_info->smbios.smbios3_phys = (uint64_t)ct->vendor_table;
            efi_print("  SMBIOS 3.0: found\n");
        }
    }

    /* ── 4. Load KERNEL.BIN from ESP ─────────────────────────────────────── */
    efi_print("  Loading KERNEL.BIN...\n");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    status = bs->locate_protocol(&SFS_GUID, NULL, (void **)&sfs);
    if (status != EFI_SUCCESS || !sfs) {
        efi_print("ERROR: Cannot locate Simple File System Protocol\n");
        return status;
    }

    EFI_FILE_PROTOCOL *root_dir = NULL;
    status = sfs->open_volume(sfs, &root_dir);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Cannot open ESP volume\n");
        return status;
    }

    /* Open \EFI\HELIOS\KERNEL.BIN */
    CHAR16 kernel_path[] = u"\\EFI\\HELIOS\\KERNEL.BIN";
    EFI_FILE_PROTOCOL *kernel_file = NULL;
    status = root_dir->open(root_dir, &kernel_file, kernel_path,
                             EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Cannot open \\EFI\\HELIOS\\KERNEL.BIN\n");
        return status;
    }

    /* Get file size */
    uint8_t file_info_buf[256];
    UINTN file_info_size = sizeof(file_info_buf);
    status = kernel_file->get_info(kernel_file, &FILE_INFO_GUID,
                                    &file_info_size, file_info_buf);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Cannot get kernel file info\n");
        return status;
    }
    EFI_FILE_INFO *finfo = (EFI_FILE_INFO *)file_info_buf;
    UINTN kernel_size = finfo->file_size;

    /* Allocate pages for the kernel */
    UINTN kernel_pages = (kernel_size + 4095) / 4096;
    uint64_t kernel_phys = 0;
    status = bs->allocate_pages(ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                 kernel_pages, &kernel_phys);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Cannot allocate memory for kernel\n");
        return status;
    }

    /* Read kernel into memory */
    UINTN read_size = kernel_size;
    status = kernel_file->read(kernel_file, &read_size, (void *)kernel_phys);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Cannot read kernel file\n");
        return status;
    }
    kernel_file->close(kernel_file);
    root_dir->close(root_dir);

    boot_info->kernel.phys_base    = kernel_phys;
    boot_info->kernel.size         = kernel_size;
    boot_info->kernel.entry_offset = 0;  /* kernel_entry is at the start */

    efi_print("  KERNEL.BIN loaded OK\n");

    /* ── 5. Get memory map & ExitBootServices ────────────────────────────── */
    efi_print("  Getting memory map...\n");

    /* Allocate space for Helios-native memory map entries */
    uint64_t helios_mmap_phys = 0;
    status = bs->allocate_pages(ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                 4, &helios_mmap_phys);  /* 16 KiB */
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Cannot allocate memory map buffer\n");
        return status;
    }

    /* Get the UEFI memory map — may need to retry after ExitBootServices */
    uint8_t mmap_buf[8192];
    UINTN mmap_size = sizeof(mmap_buf);
    UINTN map_key = 0;
    UINTN desc_size = 0;
    uint32_t desc_version = 0;

    status = bs->get_memory_map(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                                 &map_key, &desc_size, &desc_version);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: GetMemoryMap failed\n");
        return status;
    }

    /* Translate UEFI memory map to Helios format */
    helios_mem_entry_t *helios_entries = (helios_mem_entry_t *)helios_mmap_phys;
    uint64_t entry_count = 0;
    uint64_t max_entries = (4 * 4096) / sizeof(helios_mem_entry_t);

    for (UINTN off = 0; off < mmap_size && entry_count < max_entries; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + off);
        helios_entries[entry_count].phys_base   = desc->physical_start;
        helios_entries[entry_count].page_count  = desc->number_of_pages;
        helios_entries[entry_count].type        = translate_mem_type(desc->type);
        helios_entries[entry_count].attributes  = (uint32_t)desc->attribute;
        entry_count++;
    }

    boot_info->memory_map.entries_phys = helios_mmap_phys;
    boot_info->memory_map.entry_count  = entry_count;
    boot_info->memory_map.entry_size   = sizeof(helios_mem_entry_t);

    /* ── 6. ExitBootServices ─────────────────────────────────────────────── */
    efi_print("  Calling ExitBootServices...\n");

    /* Must re-fetch the memory map immediately before ExitBootServices
     * because the map_key may have changed after our allocations. */
    mmap_size = sizeof(mmap_buf);
    status = bs->get_memory_map(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                                 &map_key, &desc_size, &desc_version);
    if (status != EFI_SUCCESS) {
        efi_print("ERROR: Final GetMemoryMap failed\n");
        return status;
    }

    /* Record TSC before we lose UEFI services */
    boot_info->tsc_at_exit_boot = rdtsc();

    status = bs->exit_boot_services(image_handle, map_key);
    if (status != EFI_SUCCESS) {
        /* ExitBootServices can fail if the memory map changed between
         * GetMemoryMap and ExitBootServices. Try once more. */
        mmap_size = sizeof(mmap_buf);
        bs->get_memory_map(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                            &map_key, &desc_size, &desc_version);
        status = bs->exit_boot_services(image_handle, map_key);
        if (status != EFI_SUCCESS) {
            /* Cannot print — boot services are gone or not. Just halt. */
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* ═══════════════════════════════════════════════════════════════════ */
    /* From here: NO UEFI BOOT SERVICES. No allocation, no console.      */
    /* We only have the boot_info_t and raw hardware.                     */
    /* ═══════════════════════════════════════════════════════════════════ */

    /* ── 7. Jump to kernel_entry ─────────────────────────────────────────── */
    /* The kernel is a flat binary. Entry point is at offset 0. */
    typedef void (*kernel_entry_fn)(boot_info_t *);
    kernel_entry_fn entry = (kernel_entry_fn)(kernel_phys + boot_info->kernel.entry_offset);

    /* RDI = boot_info pointer (SysV ABI first argument) */
    entry(boot_info);

    /* Should never reach here */
    for (;;) __asm__ volatile("hlt");
}
