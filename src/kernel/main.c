/**
 * @file main.c
 * @brief Helios kernel entry point — kernel_main().
 *
 * Initializes core subsystems and dumps boot information to serial.
 * This is the Phase 0 "Hello World" — proof that the UEFI bootloader
 * successfully loaded the kernel and transferred control.
 */

#include <helios/boot_info.h>
#include <helios/capability.h>
#include <helios/pmm.h>
#include <helios/slab.h>
#include <helios/types.h>
#include <helios/vmm.h>

/* Forward declarations */
extern void serial_init(void);
extern void serial_puts(const char *s);
extern void serial_printf(const char *fmt, ...);
extern NORETURN void panic(const char *msg);

/* Memory type names for pretty-printing */
static const char *mem_type_names[] = {
    [HELIOS_MEM_FREE] = "FREE",
    [HELIOS_MEM_BOOTLOADER] = "BOOTLOADER",
    [HELIOS_MEM_KERNEL] = "KERNEL",
    [HELIOS_MEM_UEFI_RUNTIME] = "UEFI_RUNTIME",
    [HELIOS_MEM_ACPI_RECLAIM] = "ACPI_RECLAIM",
    [HELIOS_MEM_ACPI_NVS] = "ACPI_NVS",
    [HELIOS_MEM_MMIO] = "MMIO",
    [HELIOS_MEM_RESERVED] = "RESERVED",
    [HELIOS_MEM_UNUSABLE] = "UNUSABLE",
    [HELIOS_MEM_FRAMEBUFFER] = "FRAMEBUFFER",
    [HELIOS_MEM_BOOT_INFO] = "BOOT_INFO",
};

/**
 * @brief Main kernel entry point.
 *
 * Called from entry.asm after GDT and IDT are installed.
 *
 * @param boot_info  Pointer to the boot_info_t populated by the UEFI
 * bootloader.
 */
NORETURN void kernel_main(boot_info_t *boot_info) {
  /* ── Initialize serial debug output ──────────────────────────────────── */
  serial_init();

  /* ── Banner ──────────────────────────────────────────────────────────── */
  serial_puts("\n");
  serial_puts("  ╦ ╦╔═╗╦  ╦╔═╗╔═╗\n");
  serial_puts("  ╠═╣║╣ ║  ║║ ║╚═╗\n");
  serial_puts("  ╩ ╩╚═╝╩═╝╩╚═╝╚═╝\n");
  serial_puts("  Helios Operating System — kernel alive\n");
  serial_puts("  Phase 0: Foundation\n");
  serial_puts("\n");

  /* ── Validate boot_info magic ────────────────────────────────────────── */
  if (!boot_info) {
    panic("kernel_main: boot_info is NULL");
  }
  if (boot_info->magic != BOOT_INFO_MAGIC) {
    serial_printf("  Expected magic: 0x%016lx\n", (uint64_t)BOOT_INFO_MAGIC);
    serial_printf("  Got magic:      0x%016lx\n", boot_info->magic);
    panic("kernel_main: boot_info magic mismatch");
  }
  serial_printf("  boot_info at %p — magic OK, version %u\n\n",
                (void *)boot_info, boot_info->version);

  /* ── Framebuffer info ────────────────────────────────────────────────── */
  serial_puts("  ── Framebuffer ──────────────────────────────────────\n");
  serial_printf("  Base:       0x%016lx\n", boot_info->framebuffer.base_phys);
  serial_printf("  Resolution: %ux%u @ %ubpp\n", boot_info->framebuffer.width,
                boot_info->framebuffer.height, boot_info->framebuffer.bpp);
  serial_printf("  Pitch:      %u bytes/scanline\n",
                boot_info->framebuffer.pitch);
  serial_printf("  Pixel fmt:  R(%u<<%u) G(%u<<%u) B(%u<<%u)\n",
                boot_info->framebuffer.red_mask_size,
                boot_info->framebuffer.red_mask_shift,
                boot_info->framebuffer.green_mask_size,
                boot_info->framebuffer.green_mask_shift,
                boot_info->framebuffer.blue_mask_size,
                boot_info->framebuffer.blue_mask_shift);
  serial_puts("\n");

  /* ── Memory map ──────────────────────────────────────────────────────── */
  serial_puts("  ── Memory Map ───────────────────────────────────────\n");
  helios_mem_entry_t *entries =
      (helios_mem_entry_t *)boot_info->memory_map.entries_phys;
  uint64_t total_free = 0;
  uint64_t total_all = 0;

  serial_printf("  %lu entries:\n", boot_info->memory_map.entry_count);
  for (uint64_t i = 0; i < boot_info->memory_map.entry_count; i++) {
    const char *type_name = "UNKNOWN";
    if (entries[i].type < ARRAY_SIZE(mem_type_names) &&
        mem_type_names[entries[i].type]) {
      type_name = mem_type_names[entries[i].type];
    }

    uint64_t size_bytes = entries[i].page_count * PAGE_SIZE;
    uint64_t size_kib = size_bytes / 1024;

    serial_printf("    [%02lu] 0x%016lx  %8lu KiB  %s\n", i,
                  entries[i].phys_base, size_kib, type_name);

    total_all += size_bytes;
    if (entries[i].type == HELIOS_MEM_FREE) {
      total_free += size_bytes;
    }
  }
  serial_printf("\n  Total memory:      %lu MiB\n", total_all / (1024 * 1024));
  serial_printf("  Free (usable):     %lu MiB\n", total_free / (1024 * 1024));
  serial_puts("\n");

  /* ── ACPI ────────────────────────────────────────────────────────────── */
  serial_puts("  ── ACPI ─────────────────────────────────────────────\n");
  serial_printf("  RSDP at: 0x%016lx\n", boot_info->acpi.rsdp_phys);
  serial_puts("\n");

  /* ── Kernel image ────────────────────────────────────────────────────── */
  serial_puts("  ── Kernel Image ─────────────────────────────────────\n");
  serial_printf("  Loaded at:    0x%016lx\n", boot_info->kernel.phys_base);
  serial_printf("  Size:         %lu bytes (%lu KiB)\n", boot_info->kernel.size,
                boot_info->kernel.size / 1024);
  serial_printf("  Entry offset: 0x%lx\n", boot_info->kernel.entry_offset);
  serial_puts("\n");

  /* ── TSC ─────────────────────────────────────────────────────────────── */
  serial_printf("  TSC at ExitBootServices: %lu\n",
                boot_info->tsc_at_exit_boot);
  serial_printf("  TSC now:                 %lu\n", rdtsc());
  serial_puts("\n");

  /* ── Phase 0 complete ────────────────────────────────────────────────── */
  serial_puts("  ════════════════════════════════════════════════════\n");
  serial_puts("  Phase 0 COMPLETE — kernel is alive.\n");
  serial_puts("  Next: Phase 1 — Memory Management\n");
  serial_puts("  ════════════════════════════════════════════════════\n");
  serial_puts("\n");

  /* ── Phase 1: Memory Management ───────────────────────────── */
  serial_puts("  ── Phase 1: Memory Management ──────────────────────\n");

  /* Step 1: PMM must be first */
  pmm_init(boot_info);
  serial_printf("  PMM: %lu MiB free\n",
                pmm_free_page_count() * PAGE_SIZE / (1024 * 1024));

  /* Step 2: SASOS paging — replaces bootstrap tables */
  vmm_init_sasos(boot_info);
  serial_puts("  VMM: SASOS PML4 active, bootstrap tables retired\n");
  serial_puts("  VMM: NX/PCID/SMEP/SMAP enabled where supported\n");
  serial_puts("  VMM: Guard page installed below kernel stack\n");

  /* Step 2b: Reclaim bootloader-allocated physical pages now that the
   * direct map is live.  vmm_init_sasos() calls pmm_set_direct_map_active()
   * internally, so this is safe to call immediately after. */
  pmm_reclaim_bootloader();
  serial_printf("  PMM: %lu MiB free after reclaim\n",
                pmm_free_page_count() * PAGE_SIZE / (1024 * 1024));

  /* Step 3: Slab */
  slab_init();
  serial_puts("  SLAB: allocator online\n");

  /* Step 4: Capability manager */
  cap_manager_init();
  serial_puts("  CAP: capability manager online\n");

  /* Smoke-test capability round-trip */
  cap_token_t test_cap =
      cap_create(0x1000, 0x1000, CAP_PERM_READ | CAP_PERM_WRITE, 0);
  if (!cap_validate(&test_cap))
    panic("Phase 1: capability HMAC self-test FAILED");
  serial_puts("  CAP: HMAC self-test PASSED\n");

  /* Phase 1 complete */
  serial_puts("\n");
  serial_puts("  ════════════════════════════════════════════════════\n");
  serial_puts("  Phase 1 COMPLETE — memory management online.\n");
  serial_puts("  Next: Phase 2 — SMP & Scheduler\n");
  serial_puts("  ════════════════════════════════════════════════════\n\n");

  /* ── Idle halt loop ──────────────────────────────────────────────────── */
  serial_puts("  Entering idle halt loop. Connect GDB or reset.\n\n");
  for (;;) {
    cpu_halt();
  }
}
