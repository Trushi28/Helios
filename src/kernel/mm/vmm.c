#include <arch/x86_64/cpuid.h>
#include <arch/x86_64/msr.h>
#include <arch/x86_64/paging.h>
#include <helios/pmm.h>
#include <helios/types.h>
#include <helios/vmm.h>

extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern NORETURN void panic(const char *msg);
extern void *memset(void *dest, int val, size_t n);

extern char kernel_stack_guard[];  /* dedicated, reserved guard page (entry.asm) */
extern char kernel_stack_bottom[];
extern char __kernel_end[]; /* linker symbol — true end of BSS */

static phys_addr_t g_pml4_phys;
static virt_addr_t g_guard_page_addr;
static virt_addr_t g_slab_heap_cursor = KERNEL_HEAP_BASE;
static bool g_sasos_active = false;
static bool g_nx_supported = false;

bool vmm_nx_supported(void) { return g_nx_supported; }

/* Section-boundary symbols provided by linker script */
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __kernel_end[];    /* covers .data + .bss */
extern char kernel_stack_bottom[];

static inline void *phys_to_virt_vmm(phys_addr_t phys) {
  if (g_sasos_active)
    return (void *)(KERNEL_PHYS_MAP_BASE + phys);
  return (void *)(uintptr_t)phys;
}

static phys_addr_t alloc_table_page(void) {
  phys_addr_t page = pmm_alloc_pages(0);
  if (page == 0)
    panic("VMM: out of memory allocating page table");
  memset(phys_to_virt_vmm(page), 0, PAGE_SIZE);
  return page;
}

static phys_addr_t ensure_table(phys_addr_t table_phys, uint64_t index,
                                uint64_t flags) {
  uint64_t *table = (uint64_t *)phys_to_virt_vmm(table_phys);
  if (!(table[index] & PTE_PRESENT)) {
    phys_addr_t sub = alloc_table_page();
    table[index] = sub | flags | PTE_PRESENT;
  }
  return table[index] & PTE_ADDR_MASK;
}

void vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
  phys_addr_t pdpt_phys =
      ensure_table(g_pml4_phys, PML4_INDEX(virt), PTE_WRITABLE | PTE_PRESENT);
  phys_addr_t pd_phys =
      ensure_table(pdpt_phys, PDPT_INDEX(virt), PTE_WRITABLE | PTE_PRESENT);
  phys_addr_t pt_phys =
      ensure_table(pd_phys, PD_INDEX(virt), PTE_WRITABLE | PTE_PRESENT);

  uint64_t *pt = (uint64_t *)phys_to_virt_vmm(pt_phys);
  pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;
  __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_map_range(virt_addr_t virt, phys_addr_t phys, uint64_t size,
                   uint64_t flags) {
  virt_addr_t va = ALIGN_DOWN(virt, PAGE_SIZE);
  phys_addr_t pa = ALIGN_DOWN(phys, PAGE_SIZE);
  virt_addr_t end = ALIGN_UP(virt + size, PAGE_SIZE);
  while (va < end) {
    vmm_map_page(va, pa, flags);
    va += PAGE_SIZE;
    pa += PAGE_SIZE;
  }
}

void vmm_unmap_page(virt_addr_t virt) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt_vmm(g_pml4_phys);
  if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT))
    return;

  phys_addr_t pdpt_phys = pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK;
  uint64_t *pdpt = (uint64_t *)phys_to_virt_vmm(pdpt_phys);
  if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT))
    return;

  phys_addr_t pd_phys = pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK;
  uint64_t *pd = (uint64_t *)phys_to_virt_vmm(pd_phys);
  if (!(pd[PD_INDEX(virt)] & PTE_PRESENT))
    return;

  phys_addr_t pt_phys = pd[PD_INDEX(virt)] & PTE_ADDR_MASK;
  uint64_t *pt = (uint64_t *)phys_to_virt_vmm(pt_phys);
  pt[PT_INDEX(virt)] = 0;
  __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

phys_addr_t vmm_virt_to_phys(virt_addr_t virt) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt_vmm(g_pml4_phys);
  if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT))
    return 0;

  phys_addr_t pdpt_phys = pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK;
  uint64_t *pdpt = (uint64_t *)phys_to_virt_vmm(pdpt_phys);
  if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT))
    return 0;

  phys_addr_t pd_phys = pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK;
  uint64_t *pd = (uint64_t *)phys_to_virt_vmm(pd_phys);
  if (!(pd[PD_INDEX(virt)] & PTE_PRESENT))
    return 0;

  if (pd[PD_INDEX(virt)] & PTE_HUGE_PAGE) {
    phys_addr_t base = pd[PD_INDEX(virt)] & PTE_ADDR_MASK;
    return base + (virt & (PAGE_SIZE_2M - 1));
  }

  phys_addr_t pt_phys = pd[PD_INDEX(virt)] & PTE_ADDR_MASK;
  uint64_t *pt = (uint64_t *)phys_to_virt_vmm(pt_phys);
  if (!(pt[PT_INDEX(virt)] & PTE_PRESENT))
    return 0;

  return (pt[PT_INDEX(virt)] & PTE_ADDR_MASK) + (virt & (PAGE_SIZE_4K - 1));
}

virt_addr_t vmm_get_guard_page_addr(void) { return g_guard_page_addr; }

virt_addr_t vmm_heap_alloc_page(void) {
  virt_addr_t va = g_slab_heap_cursor;
  g_slab_heap_cursor += PAGE_SIZE;
  return va;
}

/**
 * @brief Check if a physical range overlaps any MMIO or Reserved region.
 *
 * Used by the direct-map builder to skip 2 MiB huge-page entries that
 * overlap device MMIO regions. Those regions must be mapped explicitly
 * by drivers with UC (uncacheable) attributes via mmio_map_bar().
 */
static bool phys_range_is_mmio(const helios_mem_entry_t *entries,
                                uint64_t count,
                                phys_addr_t base, uint64_t size) {
    phys_addr_t end = base + size;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t t = entries[i].type;
        if (t != HELIOS_MEM_MMIO && t != HELIOS_MEM_RESERVED) continue;
        phys_addr_t r_end = entries[i].phys_base + entries[i].page_count * PAGE_SIZE;
        if (entries[i].phys_base < end && r_end > base) return true;
    }
    return false;
}

void vmm_init_sasos(boot_info_t *boot_info) {
  phys_addr_t kernel_phys = boot_info->kernel.phys_base;

  g_pml4_phys = alloc_table_page();
  serial_printf("  VMM: new PML4 at phys 0x%lx\n", g_pml4_phys);

  /* ── Check and enable NX BEFORE building page tables ───────────────── */
  /* The NX bit (bit 63) in PTEs is a RESERVED bit unless EFER.NXE is set.
   * Setting a reserved PTE bit causes #PF when the CPU walks the entry.
   * We must enable NXE first, then conditionally use PTE_NO_EXECUTE. */
  {
    cpuid_result_t ext = cpuid(0x80000001);
    if (ext.edx & (1u << 20)) {
      uint64_t efer = rdmsr(MSR_IA32_EFER);
      efer |= EFER_NXE;
      wrmsr(MSR_IA32_EFER, efer);
      g_nx_supported = true;
      serial_puts("  VMM: NX (EFER.NXE) enabled\n");
    } else {
      serial_puts("  VMM: NX not supported by CPU\n");
    }
  }

  /* Map kernel with W^X per-section protection.
   * Use linker symbols for section boundaries.
   * Compute phys = kernel_phys + (section_va - KERNEL_VMA) for each region. */
  uint64_t text_flags = PTE_PRESENT | PTE_GLOBAL;
  uint64_t rodata_flags = PTE_PRESENT | PTE_GLOBAL |
                          (g_nx_supported ? PTE_NO_EXECUTE : 0);
  uint64_t data_flags = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL |
                        (g_nx_supported ? PTE_NO_EXECUTE : 0);

  /* .text — executable, not writable */
  uint64_t text_size = (uint64_t)__text_end - (uint64_t)__text_start;
  vmm_map_range((virt_addr_t)__text_start,
                kernel_phys + ((uint64_t)__text_start - KERNEL_VMA),
                text_size, text_flags);
  serial_printf("  VMM: .text   [%p, %p) RX\n", __text_start, __text_end);

  /* .rodata — not writable, NX if available */
  uint64_t rodata_size = (uint64_t)__data_start - (uint64_t)__rodata_start;
  vmm_map_range((virt_addr_t)__rodata_start,
                kernel_phys + ((uint64_t)__rodata_start - KERNEL_VMA),
                rodata_size, rodata_flags);
  serial_printf("  VMM: .rodata [%p, %p) R%s\n", __rodata_start, __data_start,
                g_nx_supported ? "+NX" : "");

  /* .data + .bss + stack — writable, NX if available */
  uint64_t data_size = (uint64_t)__kernel_end - (uint64_t)__data_start;
  vmm_map_range((virt_addr_t)__data_start,
                kernel_phys + ((uint64_t)__data_start - KERNEL_VMA),
                data_size, data_flags);
  serial_printf("  VMM: .data   [%p, %p) RW%s\n", __data_start, __kernel_end,
                g_nx_supported ? "+NX" : "");

  uint64_t kernel_pages = ((uint64_t)__kernel_end - KERNEL_VMA + PAGE_SIZE - 1) / PAGE_SIZE;
  serial_printf("  VMM: kernel mapped %lu pages at KERNEL_VMA 0x%lx (W^X)\n",
                kernel_pages, (uint64_t)KERNEL_VMA);

  /* Physical direct map — 2 MiB huge pages up to top of RAM.
   * Skip MMIO/Reserved regions — those are mapped by drivers with UC. */
  helios_mem_entry_t *entries =
      (helios_mem_entry_t *)(uintptr_t)boot_info->memory_map.entries_phys;
  uint64_t entry_count = boot_info->memory_map.entry_count;
  phys_addr_t max_phys = 0;

  for (uint64_t i = 0; i < entry_count; i++) {
    uint32_t t = entries[i].type;
    if (t == HELIOS_MEM_FREE || t == HELIOS_MEM_BOOTLOADER ||
        t == HELIOS_MEM_KERNEL || t == HELIOS_MEM_BOOT_INFO ||
        t == HELIOS_MEM_ACPI_RECLAIM || t == HELIOS_MEM_ACPI_NVS ||
        t == HELIOS_MEM_UEFI_RUNTIME) {
      phys_addr_t end =
          entries[i].phys_base + entries[i].page_count * PAGE_SIZE;
      if (end > max_phys)
        max_phys = end;
    }
  }

  uint64_t total_phys_mem = ALIGN_UP(max_phys, PAGE_SIZE_2M);
  uint64_t huge_page_count = total_phys_mem / PAGE_SIZE_2M;
  uint64_t direct_flags =
      PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_HUGE_PAGE;
  if (g_nx_supported) {
    direct_flags |= PTE_NO_EXECUTE;
  }

  serial_printf("  VMM: direct map: %lu MiB (%lu x 2MiB pages)\n",
                total_phys_mem / (1024 * 1024), huge_page_count);

  for (uint64_t i = 0; i < huge_page_count; i++) {
    phys_addr_t pa = i * PAGE_SIZE_2M;
    if (phys_range_is_mmio(entries, entry_count, pa, PAGE_SIZE_2M)) continue;
    virt_addr_t va = KERNEL_PHYS_MAP_BASE + pa;

    phys_addr_t pdpt_phys =
        ensure_table(g_pml4_phys, PML4_INDEX(va), PTE_WRITABLE | PTE_PRESENT);
    phys_addr_t pd_phys =
        ensure_table(pdpt_phys, PDPT_INDEX(va), PTE_WRITABLE | PTE_PRESENT);

    if (pa & (PAGE_SIZE_2M - 1))
      panic("VMM: 2 MiB PDE physical address not aligned");

    uint64_t *pd = (uint64_t *)phys_to_virt_vmm(pd_phys);
    pd[PD_INDEX(va)] = (pa & PTE_ADDR_MASK) | direct_flags;
  }
  /* ── CR3 switch ──────────────────────────────────────────────────────
   * The kernel is executing from IDENTITY-MAPPED physical addresses
   * (entry.asm's RIP-relative calls resolve to physical addresses, and
   * the entire C call stack has physical return addresses and saved RBP
   * values). The new PML4 must include the identity map or the first
   * instruction fetch / stack access after CR3 load will #PF.
   *
   * We copy the bootstrap PML4[0] entry (which maps 0-512 GiB via
   * boot_pdpt_low) into the new PML4. This identity map uses low
   * canonical addresses (user half) which is harmless in Phase 1
   * since there are no user-space processes. It will be removed when
   * user-space support is added in a later phase.
   * ─────────────────────────────────────────────────────────────────── */
  serial_puts("  VMM: loading new CR3...\n");

  /* Copy identity-map entry from old PML4[0] into new PML4[0] */
  {
    phys_addr_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    uint64_t *old_pml4 = (uint64_t *)(uintptr_t)(old_cr3 & PTE_ADDR_MASK);
    uint64_t *new_pml4 = (uint64_t *)phys_to_virt_vmm(g_pml4_phys);
    new_pml4[0] = old_pml4[0];
  }

  /* Load new CR3 — identity map + KERNEL_VMA + direct map all present */
  __asm__ volatile("mov %0, %%cr3" : : "r"(g_pml4_phys) : "memory");
  g_sasos_active = true;
  serial_puts("  VMM: SASOS PML4 active\n");

  pmm_set_direct_map_active();

  /* PCID */
  {
    cpuid_result_t r = cpuid(1);
    if (r.ecx & (1u << 17)) {
      write_cr4(read_cr4() | CR4_PCIDE);
      serial_puts("  VMM: PCID enabled\n");
    }
  }

  /* SMEP */
  {
    cpuid_result_t r = cpuid_count(7, 0);
    if (r.ebx & CPUID_7_EBX_SMEP) {
      write_cr4(read_cr4() | CR4_SMEP);
      serial_puts("  VMM: SMEP enabled\n");
    }
  }

  /* SMAP */
  {
    cpuid_result_t r = cpuid_count(7, 0);
    if (r.ebx & CPUID_7_EBX_SMAP) {
      write_cr4(read_cr4() | CR4_SMAP);
      serial_puts("  VMM: SMAP enabled\n");
    }
  }

  /* Guard page below kernel stack.
   *
   * kernel_stack_guard is an explicitly reserved 4 KiB region in .bss
   * (see entry.asm) placed immediately below kernel_stack_bottom. This
   * is deliberately NOT computed as "kernel_stack_bottom - PAGE_SIZE":
   * that expression is only as safe as whatever the linker happens to
   * place there, and once .data/.bss held live content (e.g. panic.c's
   * __stack_chk_guard) it silently unmapped that content instead of an
   * empty page, corrupting the stack-protector canary and turning every
   * subsequent protected function call into a fresh page fault.
   *
   * The assertion below fails fast (at boot, on serial) if the two
   * symbols ever drift apart instead of silently unmapping the wrong
   * page again. */
  if ((virt_addr_t)kernel_stack_guard + PAGE_SIZE != (virt_addr_t)kernel_stack_bottom) {
    panic("VMM: kernel_stack_guard is not immediately below kernel_stack_bottom");
  }

  g_guard_page_addr = (virt_addr_t)kernel_stack_guard;
  vmm_unmap_page(g_guard_page_addr);
  serial_printf("  VMM: guard page at 0x%lx (reserved, below kernel stack)\n",
                g_guard_page_addr);
}
