/**
 * @file pmm.h
 * @brief Helios OS — Physical Memory Manager (buddy allocator).
 *
 * Order 0 = 4 KiB (1 page), up to Order 10 = 4 MiB (1024 pages).
 * Phase 1: single zone, no NUMA awareness.
 */

#ifndef HELIOS_PMM_H
#define HELIOS_PMM_H

#include <helios/boot_info.h>
#include <helios/types.h>

#define PMM_MAX_ORDER 10

/**
 * @brief Initialize the physical memory manager from the UEFI memory map.
 *
 * Must be called before vmm_init_sasos(). Uses a bootstrap bump allocator
 * internally to allocate the bitmap.
 */
void pmm_init(boot_info_t *boot_info);

/**
 * @brief Allocate 2^order contiguous physical pages.
 * @param order  Allocation order (0 = 4 KiB, 10 = 4 MiB).
 * @return Physical address of the allocated block, or 0 on failure.
 */
phys_addr_t pmm_alloc_pages(uint32_t order);

/**
 * @brief Free a previously allocated block of 2^order pages.
 * @param addr   Physical address (must be aligned to 2^order pages).
 * @param order  The order that was used to allocate the block.
 */
void pmm_free_pages(phys_addr_t addr, uint32_t order);

/**
 * @brief Return the number of free pages currently available.
 */
uint64_t pmm_free_page_count(void);

/**
 * @brief Return the total number of pages managed by the PMM.
 */
uint64_t pmm_total_page_count(void);

/**
 * @brief Print PMM statistics to serial.
 * Called after SASOS paging is established.
 */
void pmm_dump_stats(void);

/**
 * @brief Notify the PMM that the physical direct map is now active.
 *
 * After this call, the PMM accesses physical memory through
 * KERNEL_PHYS_MAP_BASE + phys_addr instead of direct identity-mapped access.
 */
void pmm_set_direct_map_active(void);

/**
 * @brief Reclaim bootloader-allocated physical memory.
 *
 * Returns all HELIOS_MEM_BOOTLOADER pages (minus the kernel image and
 * PMM bitmap) to the buddy allocator.
 *
 * Call after vmm_init_sasos() and pmm_set_direct_map_active().
 */
void pmm_reclaim_bootloader(void);

/**
 * @brief Reclaim ACPI reclaimable memory.
 *
 * Returns all HELIOS_MEM_ACPI_RECLAIM pages to the buddy allocator.
 * Call after ACPI tables have been fully parsed (Phase 2).
 */
void pmm_reclaim_acpi(void);

#endif /* HELIOS_PMM_H */
