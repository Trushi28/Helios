/**
 * @file vmm.h
 * @brief Helios OS — Virtual Memory Manager (SASOS page tables).
 *
 * Manages the single address space page tables. After vmm_init_sasos(),
 * the bootstrap identity map is replaced with the proper SASOS layout.
 */

#ifndef HELIOS_VMM_H
#define HELIOS_VMM_H

#include <helios/boot_info.h>
#include <helios/types.h>

/**
 * @brief Build and activate the SASOS page tables.
 *
 * Replaces bootstrap page tables. After this call:
 *   - Kernel code is mapped at KERNEL_VMA
 *   - Physical direct map is at KERNEL_PHYS_MAP_BASE
 *   - Guard page is installed below kernel stack
 *   - NX, PCID, SMEP, SMAP are enabled (where supported)
 *
 * Must be called after pmm_init() and before slab_init().
 */
void vmm_init_sasos(boot_info_t *boot_info);

/**
 * @brief Map a single 4 KiB page.
 *
 * Creates intermediate PML4/PDPT/PD/PT entries as needed (allocated from PMM).
 *
 * @param virt   Virtual address (must be page-aligned).
 * @param phys   Physical address (must be page-aligned).
 * @param flags  PTE flags (PTE_PRESENT, PTE_WRITABLE, etc.).
 */
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint64_t flags);

/**
 * @brief Map a contiguous range of 4 KiB pages.
 *
 * Calls vmm_map_page() for each page in [virt, virt+size).
 * Both virt and phys are rounded down; size is rounded up.
 *
 * @param virt   Virtual base address.
 * @param phys   Physical base address.
 * @param size   Size in bytes (rounded up to page boundary).
 * @param flags  PTE flags applied to every page in the range.
 */
void vmm_map_range(virt_addr_t virt, phys_addr_t phys, uint64_t size,
                   uint64_t flags);

/**
 * @brief Unmap a single 4 KiB page (set PTE to 0, INVLPG).
 *
 * @param virt   Virtual address of the page to unmap.
 */
void vmm_unmap_page(virt_addr_t virt);

/**
 * @brief Walk the page tables to translate a virtual address to physical.
 *
 * @param virt   Virtual address to translate.
 * @return Physical address, or 0 if not mapped.
 */
phys_addr_t vmm_virt_to_phys(virt_addr_t virt);

/**
 * @brief Get the virtual address of the guard page below the kernel stack.
 *
 * Used by the #PF handler to detect kernel stack overflow.
 */
virt_addr_t vmm_get_guard_page_addr(void);

/**
 * @brief Allocate the next virtual address in the kernel heap region.
 *
 * Used by the slab allocator to map new slab pages.
 * Returns a page-aligned virtual address in [KERNEL_HEAP_BASE, ...).
 */
virt_addr_t vmm_heap_alloc_page(void);

/** Returns true if the CPU supports and has enabled the NX bit (EFER.NXE = 1). */
bool vmm_nx_supported(void);

#endif /* HELIOS_VMM_H */
