/**
 * @file pmm.c
 * @brief Helios OS — Physical Memory Manager (buddy allocator).
 *
 * Implements a buddy allocator with orders 0 (4 KiB) through 10 (4 MiB).
 * Free list nodes are stored intrusively in the physical frames themselves.
 * A flat bitmap (1 bit per page) tracks allocation state.
 *
 * Initialization:
 *   1. A bootstrap bump allocator carves memory for the bitmap from the
 *      first large HELIOS_MEM_FREE region.
 *   2. The UEFI memory map is parsed to identify free regions.
 *   3. Free regions are broken into buddy blocks and inserted into free lists.
 *
 * After vmm_init_sasos() activates the SASOS page tables, the PMM is
 * notified via pmm_set_direct_map_active() so it accesses physical memory
 * through KERNEL_PHYS_MAP_BASE instead of identity-mapped addresses.
 */

#include <arch/x86_64/paging.h>
#include <helios/boot_info.h>
#include <helios/pmm.h>
#include <helios/spinlock.h>
#include <helios/types.h>

#ifndef UNIT_TEST
extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern NORETURN void panic(const char *msg);
extern void *memset(void *dest, int val, size_t n);
#else
/* Unit test stubs — provided by test harness */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define serial_printf(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define serial_puts(s) printf("%s", s)
#define panic(msg)                                                             \
  do {                                                                         \
    fprintf(stderr, "PANIC: %s\n", msg);                                       \
    exit(1);                                                                   \
  } while (0)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Internal types                                                            */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Intrusive free list node stored in the first bytes of a free frame.
 */
typedef struct free_node {
  struct free_node *next;
  struct free_node *prev;
} free_node_t;

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  PMM global state                                                          */
/* ═══════════════════════════════════════════════════════════════════════════
 */

static struct {
  spinlock_t lock;
  free_node_t *free_lists[PMM_MAX_ORDER + 1];
  uint8_t *bitmap;
  phys_addr_t
      bitmap_phys;      /* physical address of bitmap (for direct-map fixup) */
  uint64_t bitmap_size; /* in bytes */
  uint64_t total_pages;
  uint64_t free_pages;
  phys_addr_t max_phys_addr; /* highest physical address */
  bool direct_map_active;
  /* Saved during init for use by reclaim functions */
  phys_addr_t saved_entries_phys; /* physical address of memory map entries */
  uint64_t saved_entry_count;
  phys_addr_t kernel_phys_base; /* kernel image start */
  phys_addr_t kernel_phys_end;  /* kernel image end   */
} g_pmm;

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Physical-to-virtual pointer conversion                                    */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Convert a physical address to a usable pointer.
 *
 * Before the SASOS direct map is active, physical addresses are accessed
 * via the identity map (UEFI + bootstrap tables). After vmm_init_sasos(),
 * access is through KERNEL_PHYS_MAP_BASE + phys.
 */
static inline void *phys_to_virt(phys_addr_t phys) {
#ifdef UNIT_TEST
  /* In unit tests, g_fake_mem is the "physical memory" */
  extern uint8_t *g_fake_phys_base;
  return (void *)(g_fake_phys_base + phys);
#else
  if (g_pmm.direct_map_active) {
    return (void *)(KERNEL_PHYS_MAP_BASE + phys);
  }
  return (void *)phys;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Bitmap helpers                                                            */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Convert a physical address to a page frame number.
 */
static inline uint64_t phys_to_pfn(phys_addr_t addr) {
  return addr / PAGE_SIZE;
}

/**
 * @brief Set a bit in the bitmap (mark page as allocated).
 */
static inline void bitmap_set(uint64_t pfn) {
  if (pfn / 8 < g_pmm.bitmap_size) {
    g_pmm.bitmap[pfn / 8] |= (1u << (pfn % 8));
  }
}

/**
 * @brief Clear a bit in the bitmap (mark page as free).
 */
static inline void bitmap_clear(uint64_t pfn) {
  if (pfn / 8 < g_pmm.bitmap_size) {
    g_pmm.bitmap[pfn / 8] &= ~(1u << (pfn % 8));
  }
}

/**
 * @brief Test a bit in the bitmap.
 * @return true if the page is allocated (bit set), false if free.
 */
static inline bool bitmap_test(uint64_t pfn) {
  if (pfn / 8 < g_pmm.bitmap_size) {
    return (g_pmm.bitmap[pfn / 8] >> (pfn % 8)) & 1;
  }
  return true; /* out of range = treated as allocated */
}

/**
 * @brief Mark a range of pages as allocated in the bitmap.
 */
static void bitmap_set_range(uint64_t pfn_start, uint64_t count) {
  for (uint64_t i = 0; i < count; i++) {
    bitmap_set(pfn_start + i);
  }
}

/**
 * @brief Mark a range of pages as free in the bitmap.
 */
static void bitmap_clear_range(uint64_t pfn_start, uint64_t count) {
  for (uint64_t i = 0; i < count; i++) {
    bitmap_clear(pfn_start + i);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Free list helpers                                                         */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Insert a block at the head of a free list.
 */
static void free_list_push(uint32_t order, phys_addr_t addr) {
  free_node_t *node = (free_node_t *)phys_to_virt(addr);
  node->prev = NULL;
  node->next = g_pmm.free_lists[order];
  if (g_pmm.free_lists[order]) {
    g_pmm.free_lists[order]->prev = node;
  }
  g_pmm.free_lists[order] = node;
}

/**
 * @brief Remove a specific block from a free list.
 */
static void free_list_remove(uint32_t order, phys_addr_t addr) {
  free_node_t *node = (free_node_t *)phys_to_virt(addr);
  if (node->prev) {
    node->prev->next = node->next;
  } else {
    g_pmm.free_lists[order] = node->next;
  }
  if (node->next) {
    node->next->prev = node->prev;
  }
  node->next = NULL;
  node->prev = NULL;
}

/**
 * @brief Pop the first block from a free list. Returns 0 if empty.
 */
static phys_addr_t free_list_pop(uint32_t order) {
  free_node_t *node = g_pmm.free_lists[order];
  if (!node) {
    return 0;
  }

  /* Convert the virtual pointer back to a physical address */
#ifdef UNIT_TEST
  extern uint8_t *g_fake_phys_base;
  phys_addr_t addr = (phys_addr_t)((uint8_t *)node - g_fake_phys_base);
#else
  phys_addr_t addr;
  if (g_pmm.direct_map_active) {
    addr = (phys_addr_t)((uint64_t)node - KERNEL_PHYS_MAP_BASE);
  } else {
    addr = (phys_addr_t)(uint64_t)node;
  }
#endif

  g_pmm.free_lists[order] = node->next;
  if (node->next) {
    node->next->prev = NULL;
  }
  node->next = NULL;
  node->prev = NULL;

  return addr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Bootstrap bump allocator                                                  */
/* ═══════════════════════════════════════════════════════════════════════════
 */

static struct {
  phys_addr_t base;
  phys_addr_t current;
  phys_addr_t end;
} g_bootstrap;

/**
 * @brief Allocate n_pages of physically contiguous memory from the
 *        bootstrap bump allocator. Used ONLY during pmm_init() for
 *        the bitmap allocation. Returns physical address.
 */
static phys_addr_t pmm_bootstrap_alloc(uint64_t n_pages) {
  phys_addr_t alloc = g_bootstrap.current;
  phys_addr_t new_current = alloc + n_pages * PAGE_SIZE;
  if (new_current > g_bootstrap.end) {
    panic("PMM bootstrap: out of memory for bitmap allocation");
  }
  g_bootstrap.current = new_current;
  return alloc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Buddy block insertion                                                     */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Add a single free region to the buddy free lists.
 *
 * Breaks the region [base, base + size) into maximal power-of-two
 * aligned buddy blocks and inserts them into the appropriate free lists.
 */
static void pmm_add_region(phys_addr_t base, uint64_t size) {
  phys_addr_t addr = base;
  phys_addr_t end = base + size;

  while (addr < end) {
    /* Find the largest order block that:
     * 1. Starts at addr (alignment: addr must be aligned to block size)
     * 2. Fits within [addr, end) */
    uint32_t order = 0;
    for (uint32_t o = PMM_MAX_ORDER; o > 0; o--) {
      uint64_t block_size = (uint64_t)PAGE_SIZE << o;
      if ((addr & (block_size - 1)) == 0 && addr + block_size <= end) {
        order = o;
        break;
      }
    }

    /* For order 0, just check that a single page fits */
    uint64_t block_size = (uint64_t)PAGE_SIZE << order;
    if (addr + block_size > end) {
      break; /* remaining space is smaller than a page */
    }

    /* Mark pages as free in bitmap and add to free list */
    uint64_t pfn = phys_to_pfn(addr);
    uint64_t n_pages = 1ULL << order;
    bitmap_clear_range(pfn, n_pages);
    free_list_push(order, addr);
    g_pmm.free_pages += n_pages;

    addr += block_size;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  PMM initialization                                                        */
/* ═══════════════════════════════════════════════════════════════════════════
 */

void pmm_init(boot_info_t *boot_info) {
  /* Zero out PMM state */
  g_pmm.lock = (spinlock_t)SPINLOCK_INIT;
  for (uint32_t i = 0; i <= PMM_MAX_ORDER; i++) {
    g_pmm.free_lists[i] = NULL;
  }
  g_pmm.total_pages = 0;
  g_pmm.free_pages = 0;
  g_pmm.max_phys_addr = 0;
  g_pmm.direct_map_active = false;

  /* ── Pass 1: Find the highest physical address and set up bootstrap ── */
  helios_mem_entry_t *entries =
      (helios_mem_entry_t *)(uintptr_t)boot_info->memory_map.entries_phys;
  uint64_t entry_count = boot_info->memory_map.entry_count;

  phys_addr_t bootstrap_base = 0;
  uint64_t bootstrap_size = 0;

  for (uint64_t i = 0; i < entry_count; i++) {
    phys_addr_t region_end =
        entries[i].phys_base + entries[i].page_count * PAGE_SIZE;
    if (region_end > g_pmm.max_phys_addr) {
      g_pmm.max_phys_addr = region_end;
    }

    /* Find the first large HELIOS_MEM_FREE region for the bootstrap allocator
     */
    if (entries[i].type == HELIOS_MEM_FREE &&
        entries[i].page_count * PAGE_SIZE > bootstrap_size) {
      /* Skip low 1 MiB */
      phys_addr_t base = entries[i].phys_base;
      uint64_t size = entries[i].page_count * PAGE_SIZE;
      if (base < 0x100000) {
        uint64_t skip = 0x100000 - base;
        if (skip >= size)
          continue;
        base += skip;
        size -= skip;
      }
      if (size > bootstrap_size) {
        bootstrap_base = base;
        bootstrap_size = size;
      }
    }
  }

  if (bootstrap_base == 0) {
    panic("PMM: no free memory region found for bootstrap");
  }

  /* Set up bootstrap bump allocator */
  g_bootstrap.base = bootstrap_base;
  g_bootstrap.current = bootstrap_base;
  g_bootstrap.end = bootstrap_base + bootstrap_size;

  /* ── Allocate bitmap ─────────────────────────────────────────────────── */
  uint64_t total_possible_pages = g_pmm.max_phys_addr / PAGE_SIZE;
  g_pmm.bitmap_size = (total_possible_pages + 7) / 8;
  uint64_t bitmap_pages = (g_pmm.bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

  phys_addr_t bitmap_phys = pmm_bootstrap_alloc(bitmap_pages);
  g_pmm.bitmap = (uint8_t *)phys_to_virt(bitmap_phys);
  g_pmm.bitmap_phys = bitmap_phys; /* saved for pmm_set_direct_map_active() */

  /* Save kernel bounds and memory map location for reclaim functions */
  g_pmm.saved_entries_phys = boot_info->memory_map.entries_phys;
  g_pmm.saved_entry_count = boot_info->memory_map.entry_count;
  g_pmm.kernel_phys_base = boot_info->kernel.phys_base;
  g_pmm.kernel_phys_end = boot_info->kernel.phys_base + boot_info->kernel.size;

  /* Initialize all bits to 1 (all pages start as "allocated"/reserved) */
  memset(g_pmm.bitmap, 0xFF, g_pmm.bitmap_size);

  /* ── Pass 2: Add free regions to buddy allocator ──────────────────── */
  for (uint64_t i = 0; i < entry_count; i++) {
    phys_addr_t region_base = entries[i].phys_base;
    uint64_t region_size = entries[i].page_count * PAGE_SIZE;
    phys_addr_t region_end = region_base + region_size;

    if (entries[i].type == HELIOS_MEM_FREE) {
      /* Skip the low 1 MiB unconditionally */
      if (region_base < 0x100000) {
        uint64_t skip = 0x100000 - region_base;
        if (skip >= region_size)
          continue;
        region_base += skip;
        region_size -= skip;
      }

      /* Skip the bootstrap allocator's consumed pages */
      phys_addr_t bootstrap_end = g_bootstrap.current;
      if (region_base < bootstrap_end && region_end > g_bootstrap.base) {
        /* The bootstrap region overlaps this free region.
         * Carve out the bootstrap area. */
        if (region_base >= g_bootstrap.base && region_end <= bootstrap_end) {
          /* Entire region consumed by bootstrap */
          continue;
        }
        if (region_base < g_bootstrap.base) {
          /* Add the part before bootstrap */
          uint64_t before_size = g_bootstrap.base - region_base;
          if (before_size >= PAGE_SIZE) {
            g_pmm.total_pages += before_size / PAGE_SIZE;
            pmm_add_region(region_base, before_size);
          }
        }
        if (region_end > bootstrap_end) {
          /* Add the part after bootstrap */
          phys_addr_t after_base = ALIGN_UP(bootstrap_end, PAGE_SIZE);
          if (after_base < region_end) {
            uint64_t after_size = region_end - after_base;
            g_pmm.total_pages += after_size / PAGE_SIZE;
            pmm_add_region(after_base, after_size);
          }
        }
        continue;
      }

      /* Normal free region — add it entirely */
      g_pmm.total_pages += region_size / PAGE_SIZE;
      pmm_add_region(region_base, region_size);

    } else if (entries[i].type == HELIOS_MEM_BOOTLOADER) {
      /* Reclaimable after boot — pmm_reclaim_bootloader() handles these.
       * Pages stay marked allocated in bitmap (initialized to 0xFF).
       * total_pages will be updated when they are actually reclaimed. */
    } else if (entries[i].type == HELIOS_MEM_ACPI_RECLAIM) {
      /* Reclaimable after ACPI tables are parsed — pmm_reclaim_acpi().
       * Same treatment: counted and freed at reclaim time. */
    } else if (entries[i].type == HELIOS_MEM_UNUSABLE) {
      /* Skip entirely — do not count as total pages */
    } else {
      /* UEFI_RUNTIME, ACPI_NVS, MMIO, RESERVED, FRAMEBUFFER,
       * BOOT_INFO, KERNEL — permanently reserved. */
      g_pmm.total_pages += region_size / PAGE_SIZE;
      /* Pages remain marked as allocated in bitmap */
    }
  }

  serial_printf("  PMM: bitmap at 0x%llx, %llu bytes (%llu pages)\n",
                (unsigned long long)bitmap_phys,
                (unsigned long long)g_pmm.bitmap_size,
                (unsigned long long)bitmap_pages);
  serial_printf("  PMM: max physical address: 0x%llx\n",
                (unsigned long long)g_pmm.max_phys_addr);
  serial_printf("  PMM: total pages: %llu, free pages: %llu\n",
                (unsigned long long)g_pmm.total_pages,
                (unsigned long long)g_pmm.free_pages);
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Page allocation                                                           */
/* ═══════════════════════════════════════════════════════════════════════════
 */

phys_addr_t pmm_alloc_pages(uint32_t order) {
  if (order > PMM_MAX_ORDER) {
    return 0;
  }

  spinlock_lock(&g_pmm.lock);

  /* Find the smallest order that has a free block */
  uint32_t current_order = order;
  while (current_order <= PMM_MAX_ORDER &&
         g_pmm.free_lists[current_order] == NULL) {
    current_order++;
  }

  if (current_order > PMM_MAX_ORDER) {
    spinlock_unlock(&g_pmm.lock);
    return 0; /* OOM */
  }

  /* Pop a block from the found order */
  phys_addr_t block = free_list_pop(current_order);
  uint64_t n_pages = 1ULL << current_order;
  bitmap_set_range(phys_to_pfn(block), n_pages);
  g_pmm.free_pages -= n_pages;

  /* Split the block down to the requested order */
  while (current_order > order) {
    current_order--;
    uint64_t buddy_offset = (uint64_t)PAGE_SIZE << current_order;
    phys_addr_t buddy = block + buddy_offset;

    /* The upper half becomes a free buddy */
    uint64_t buddy_pages = 1ULL << current_order;
    bitmap_clear_range(phys_to_pfn(buddy), buddy_pages);
    free_list_push(current_order, buddy);
    g_pmm.free_pages += buddy_pages;
  }

  spinlock_unlock(&g_pmm.lock);
  return block;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Page deallocation with buddy coalescing                                   */
/* ═══════════════════════════════════════════════════════════════════════════
 */

void pmm_free_pages(phys_addr_t addr, uint32_t order) {
  if (order > PMM_MAX_ORDER) {
    return;
  }
  if (addr == 0) {
    return;
  }

  spinlock_lock(&g_pmm.lock);

  uint64_t n_pages = 1ULL << order;
  bitmap_clear_range(phys_to_pfn(addr), n_pages);
  g_pmm.free_pages += n_pages;

  /* Attempt buddy coalescing */
  phys_addr_t block = addr;
  uint32_t current_order = order;

  while (current_order < PMM_MAX_ORDER) {
    /* Compute buddy address */
    uint64_t block_size = (uint64_t)PAGE_SIZE << current_order;
    phys_addr_t buddy = block ^ block_size;

    /* Check bounds */
    if (buddy + block_size > g_pmm.max_phys_addr) {
      break;
    }

    /* Check if ALL pages in the buddy block are free */
    uint64_t buddy_pfn = phys_to_pfn(buddy);
    uint64_t buddy_pages = 1ULL << current_order;
    bool buddy_free = true;
    for (uint64_t p = 0; p < buddy_pages; p++) {
      if (bitmap_test(buddy_pfn + p)) {
        buddy_free = false;
        break;
      }
    }

    if (!buddy_free) {
      break;
    }

    /* Remove buddy from its free list (pages remain free, just reorganized) */
    free_list_remove(current_order, buddy);

    /* Merge: take the lower address */
    if (buddy < block) {
      block = buddy;
    }

    current_order++;
    /* The merged block's pages are already cleared in the bitmap */
  }

  /* Insert the (possibly merged) block into the free list */
  free_list_push(current_order, block);

  spinlock_unlock(&g_pmm.lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Stats                                                                     */
/* ═══════════════════════════════════════════════════════════════════════════
 */

uint64_t pmm_free_page_count(void) { return g_pmm.free_pages; }

uint64_t pmm_total_page_count(void) { return g_pmm.total_pages; }

void pmm_set_direct_map_active(void) {
#ifndef UNIT_TEST
  /* Re-point bitmap through the direct map using the stored physical address.
   * Using the stored g_pmm.bitmap_phys is correct regardless of where UEFI
   * placed the bitmap, including above 4 GiB.  The old identity-cast trick
   * was fragile because it assumed virt == phys which only holds within the
   * 0–4 GiB window mapped by the bootstrap tables. */
  g_pmm.bitmap = (uint8_t *)(KERNEL_PHYS_MAP_BASE + g_pmm.bitmap_phys);
  g_pmm.direct_map_active = true;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Reclaim functions                                                         */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Add a sub-range of [base, end) to the free lists, excluding
 *        the kernel image and bootstrap bitmap regions.
 *
 * This is the core of bootloader reclaim: we carve out the non-reclaimable
 * pages and hand the gaps back to the buddy allocator.
 */
static void pmm_add_range_excl_reserved(phys_addr_t base, phys_addr_t end) {
  /* Exclusion zones — sorted by start address */
  phys_addr_t excl[3][2] = {
      {0, 0x100000},                                   /* low 1 MiB */
      {g_pmm.kernel_phys_base, g_pmm.kernel_phys_end}, /* kernel image */
      {g_bootstrap.base, g_bootstrap.current},         /* bitmap + bootstrap */
  };

  /* Simple insertion sort (only 3 elements) */
  for (int i = 1; i < 3; i++) {
    for (int j = i; j > 0 && excl[j - 1][0] > excl[j][0]; j--) {
      phys_addr_t tmp0 = excl[j][0];
      phys_addr_t tmp1 = excl[j][1];
      excl[j][0] = excl[j - 1][0];
      excl[j][1] = excl[j - 1][1];
      excl[j - 1][0] = tmp0;
      excl[j - 1][1] = tmp1;
    }
  }

  phys_addr_t cur = base;
  for (int i = 0; i < 3 && cur < end; i++) {
    phys_addr_t ex_start = excl[i][0];
    phys_addr_t ex_end = excl[i][1];

    if (ex_end <= cur)
      continue; /* exclusion zone is behind cursor */
    if (ex_start >= end)
      break; /* exclusion zone is beyond range   */

    /* Free [cur, ex_start) if non-empty after alignment */
    if (cur < ex_start) {
      phys_addr_t chunk_s = ALIGN_UP(cur, PAGE_SIZE);
      phys_addr_t chunk_e = ALIGN_DOWN(ex_start, PAGE_SIZE);
      if (chunk_e > chunk_s) {
        uint64_t sz = chunk_e - chunk_s;
        g_pmm.total_pages += sz / PAGE_SIZE;
        pmm_add_region(chunk_s, sz);
      }
    }
    cur = ex_end;
  }

  /* Free [cur, end) — the tail after all exclusion zones */
  if (cur < end) {
    phys_addr_t chunk_s = ALIGN_UP(cur, PAGE_SIZE);
    phys_addr_t chunk_e = ALIGN_DOWN(end, PAGE_SIZE);
    if (chunk_e > chunk_s) {
      uint64_t sz = chunk_e - chunk_s;
      g_pmm.total_pages += sz / PAGE_SIZE;
      pmm_add_region(chunk_s, sz);
    }
  }
}

/**
 * @brief Reclaim bootloader-allocated physical memory.
 *
 * Must be called after vmm_init_sasos() and pmm_set_direct_map_active().
 * Scans HELIOS_MEM_BOOTLOADER entries and returns all pages that are not
 * part of the kernel image or the PMM bitmap back to the buddy allocator.
 */
void pmm_reclaim_bootloader(void) {
  helios_mem_entry_t *entries =
      (helios_mem_entry_t *)(KERNEL_PHYS_MAP_BASE + g_pmm.saved_entries_phys);

  uint64_t reclaimed_before = g_pmm.free_pages;

  for (uint64_t i = 0; i < g_pmm.saved_entry_count; i++) {
    if (entries[i].type != HELIOS_MEM_BOOTLOADER)
      continue;

    phys_addr_t base = entries[i].phys_base;
    phys_addr_t end = base + entries[i].page_count * PAGE_SIZE;
    pmm_add_range_excl_reserved(base, end);
  }

  uint64_t reclaimed = g_pmm.free_pages - reclaimed_before;
  serial_printf("  PMM: reclaimed %llu MiB from bootloader regions\n",
                (unsigned long long)(reclaimed * PAGE_SIZE / (1024 * 1024)));
}

/**
 * @brief Reclaim ACPI reclaimable memory.
 *
 * Call after ACPI tables have been fully parsed (Phase 2).
 * ACPI_RECLAIM regions contain only ACPI table data — once parsed, the
 * physical pages can be returned to the general pool.
 */
void pmm_reclaim_acpi(void) {
  helios_mem_entry_t *entries =
      (helios_mem_entry_t *)(KERNEL_PHYS_MAP_BASE + g_pmm.saved_entries_phys);

  uint64_t reclaimed_before = g_pmm.free_pages;

  for (uint64_t i = 0; i < g_pmm.saved_entry_count; i++) {
    if (entries[i].type != HELIOS_MEM_ACPI_RECLAIM)
      continue;

    phys_addr_t base = ALIGN_UP(entries[i].phys_base, PAGE_SIZE);
    uint64_t n_pages = entries[i].page_count;
    if (n_pages == 0)
      continue;

    g_pmm.total_pages += n_pages;
    pmm_add_region(base, n_pages * PAGE_SIZE);
  }

  uint64_t reclaimed = g_pmm.free_pages - reclaimed_before;
  serial_printf("  PMM: reclaimed %llu KiB from ACPI regions\n",
                (unsigned long long)(reclaimed * PAGE_SIZE / 1024));
}

void pmm_dump_stats(void) {
  serial_puts("  PMM: Free list summary:\n");
  for (uint32_t o = 0; o <= PMM_MAX_ORDER; o++) {
    uint64_t count = 0;
    free_node_t *node = g_pmm.free_lists[o];
    while (node) {
      count++;
      node = node->next;
    }
    if (count > 0) {
      serial_printf("    Order %2u (%6llu KiB): %llu blocks\n", o,
                    (unsigned long long)((PAGE_SIZE << o) / 1024),
                    (unsigned long long)count);
    }
  }
  serial_printf(
      "  PMM: %llu / %llu pages free (%llu MiB / %llu MiB)\n",
      (unsigned long long)g_pmm.free_pages,
      (unsigned long long)g_pmm.total_pages,
      (unsigned long long)(g_pmm.free_pages * PAGE_SIZE / (1024 * 1024)),
      (unsigned long long)(g_pmm.total_pages * PAGE_SIZE / (1024 * 1024)));
}
