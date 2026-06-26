/**
 * @file slab.c
 * @brief Helios OS — Slab allocator for fixed-size kernel objects.
 *
 * Each slab = one physical page (4 KiB) mapped into the KERNEL_HEAP_BASE
 * region. A slab stores: header + free bitmap + N aligned objects.
 *
 * New slabs are obtained by calling pmm_alloc_pages(0) then
 * vmm_map_page() at the next available heap address.
 */

#include <arch/x86_64/paging.h>
#include <helios/pmm.h>
#include <helios/slab.h>
#include <helios/types.h>
#include <helios/vmm.h>

/* Forward declarations */
extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern NORETURN void panic(const char *msg);
extern void *memset(void *dest, int val, size_t n);
extern int strcmp(const char *a, const char *b);

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Cache registry                                                            */
/* ═══════════════════════════════════════════════════════════════════════════
 */

#define MAX_CACHES 16

static slab_cache_t g_caches[MAX_CACHES];
static uint32_t g_cache_count = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Slab allocation helpers                                                   */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Allocate and initialize a new slab page for a cache.
 *
 * Layout of a slab page (4 KiB):
 *   [slab_t header] [free_bitmap] [padding] [obj_0] [obj_1] ... [obj_N-1]
 */
static slab_t *slab_create_new(slab_cache_t *cache) {
  /* Allocate physical page */
  phys_addr_t phys = pmm_alloc_pages(0);
  if (phys == 0) {
    return NULL;
  }

  /* Map into kernel heap */
  virt_addr_t va = vmm_heap_alloc_page();
  vmm_map_page(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NO_EXECUTE);

  /* Initialize the slab header */
  slab_t *slab = (slab_t *)va;
  memset(slab, 0, PAGE_SIZE);

  /* Compute layout:
   * header_size = sizeof(slab_t)
   * After the header comes the bitmap, then the objects.
   * We need to figure out how many objects fit. */
  size_t obj_size = cache->obj_size;
  size_t alignment = cache->alignment;
  if (alignment < 8)
    alignment = 8;

  /* Align object size up to alignment boundary */
  size_t aligned_obj_size = ALIGN_UP(obj_size, alignment);

  /* Available space after the header */
  size_t header_size = sizeof(slab_t);
  size_t avail = PAGE_SIZE - header_size;

  /* Calculate how many objects fit.
   * For N objects, we need: ceil(N/8) bytes of bitmap + N * aligned_obj_size
   * Solve: N * aligned_obj_size + ceil(N/8) <= avail
   * Approximate: N * (aligned_obj_size + 1/8) <= avail
   * N <= avail / (aligned_obj_size + 1) (conservative)  */
  uint32_t obj_count = (uint32_t)(avail / (aligned_obj_size + 1));
  if (obj_count == 0) {
    panic("SLAB: object too large for a single page");
  }

  /* Re-check with exact bitmap size */
  uint32_t bitmap_bytes = (obj_count + 7) / 8;
  while (header_size + bitmap_bytes + (uint64_t)obj_count * aligned_obj_size >
         PAGE_SIZE) {
    obj_count--;
    bitmap_bytes = (obj_count + 7) / 8;
    if (obj_count == 0) {
      panic("SLAB: object too large for a single page");
    }
  }

  /* Set up the slab structure */
  slab->next = NULL;
  slab->obj_count = obj_count;
  slab->free_count = obj_count;

  /* The bitmap is stored right after the slab_t header (in the data[] area) */
  slab->free_bitmap = slab->data;

  /* Clear bitmap — 0 means free, 1 means allocated */
  memset(slab->free_bitmap, 0, bitmap_bytes);

  return slab;
}

/**
 * @brief Get a pointer to the Nth object in a slab.
 */
static inline void *slab_obj_ptr(slab_cache_t *cache, slab_t *slab,
                                 uint32_t index) {
  size_t alignment = cache->alignment;
  if (alignment < 8)
    alignment = 8;
  size_t aligned_obj_size = ALIGN_UP(cache->obj_size, alignment);
  uint32_t bitmap_bytes = (slab->obj_count + 7) / 8;

  /* Objects start after header + bitmap, aligned */
  uintptr_t data_start = (uintptr_t)slab->data + bitmap_bytes;
  data_start = ALIGN_UP(data_start, alignment);

  return (void *)(data_start + (size_t)index * aligned_obj_size);
}

/**
 * @brief Find the index of an object pointer within a slab.
 * @return Object index, or UINT32_MAX if not found.
 */
static inline uint32_t slab_obj_index(slab_cache_t *cache, slab_t *slab,
                                      void *obj) {
  size_t alignment = cache->alignment;
  if (alignment < 8)
    alignment = 8;
  size_t aligned_obj_size = ALIGN_UP(cache->obj_size, alignment);
  uint32_t bitmap_bytes = (slab->obj_count + 7) / 8;

  uintptr_t data_start = (uintptr_t)slab->data + bitmap_bytes;
  data_start = ALIGN_UP(data_start, alignment);

  uintptr_t offset = (uintptr_t)obj - data_start;
  if (offset % aligned_obj_size != 0) {
    return (uint32_t)-1;
  }
  uint32_t idx = (uint32_t)(offset / aligned_obj_size);
  if (idx >= slab->obj_count) {
    return (uint32_t)-1;
  }
  return idx;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Public API                                                                */
/* ═══════════════════════════════════════════════════════════════════════════
 */

slab_cache_t *slab_cache_create(const char *name, size_t obj_size,
                                size_t alignment) {
  if (g_cache_count >= MAX_CACHES) {
    panic("SLAB: too many caches");
  }

  slab_cache_t *cache = &g_caches[g_cache_count++];
  cache->name = name;
  cache->obj_size = obj_size;
  cache->alignment = alignment;
  cache->lock = (spinlock_t)SPINLOCK_INIT;
  cache->partial = NULL;
  cache->full = NULL;
  cache->empty = NULL;
  cache->total_allocs = 0;
  cache->total_frees = 0;

  return cache;
}

void *slab_alloc(slab_cache_t *cache) {
  if (!cache)
    return NULL;

  spinlock_lock(&cache->lock);

  /* Try to allocate from a partial slab first */
  slab_t *slab = cache->partial;
  if (!slab) {
    /* Try an empty slab */
    slab = cache->empty;
    if (slab) {
      cache->empty = slab->next;
      slab->next = cache->partial;
      cache->partial = slab;
    } else {
      /* Create a new slab */
      slab = slab_create_new(cache);
      if (!slab) {
        spinlock_unlock(&cache->lock);
        return NULL;
      }
      slab->next = cache->partial;
      cache->partial = slab;
    }
  }

  /* Find a free object in this slab */
  void *result = NULL;
  for (uint32_t i = 0; i < slab->obj_count; i++) {
    uint32_t byte_idx = i / 8;
    uint32_t bit_idx = i % 8;
    if (!(slab->free_bitmap[byte_idx] & (1u << bit_idx))) {
      /* Found a free slot — mark as allocated */
      slab->free_bitmap[byte_idx] |= (1u << bit_idx);
      slab->free_count--;
      result = slab_obj_ptr(cache, slab, i);
      break;
    }
  }

  /* If slab is now full, move it to the full list */
  if (slab->free_count == 0) {
    cache->partial = slab->next;
    slab->next = cache->full;
    cache->full = slab;
  }

  cache->total_allocs++;
  spinlock_unlock(&cache->lock);
  return result;
}

void slab_free(slab_cache_t *cache, void *obj) {
  if (!cache || !obj)
    return;

  spinlock_lock(&cache->lock);

  /* Determine which slab this object belongs to.
   * Slab header is at the start of the page containing the object. */
  slab_t *slab = (slab_t *)((uintptr_t)obj & PAGE_MASK);
  uint32_t idx = slab_obj_index(cache, slab, obj);

  if (idx == (uint32_t)-1) {
    spinlock_unlock(&cache->lock);
    panic("SLAB: invalid free — object does not belong to this cache");
  }

  /* Mark as free */
  uint32_t byte_idx = idx / 8;
  uint32_t bit_idx = idx % 8;
  slab->free_bitmap[byte_idx] &= ~(1u << bit_idx);

  bool was_full = (slab->free_count == 0);
  slab->free_count++;

  if (was_full) {
    /* Move from full list to partial list */
    /* Remove from full list */
    slab_t **pp = &cache->full;
    while (*pp && *pp != slab) {
      pp = &(*pp)->next;
    }
    if (*pp == slab) {
      *pp = slab->next;
    }
    slab->next = cache->partial;
    cache->partial = slab;
  } else if (slab->free_count == slab->obj_count) {
    /* Slab is now completely empty — move to empty list (cache up to 2) */
    /* Remove from partial list */
    slab_t **pp = &cache->partial;
    while (*pp && *pp != slab) {
      pp = &(*pp)->next;
    }
    if (*pp == slab) {
      *pp = slab->next;
    }

    /* Count empty slabs */
    uint32_t empty_count = 0;
    slab_t *e = cache->empty;
    while (e) {
      empty_count++;
      e = e->next;
    }

    if (empty_count < 2) {
      slab->next = cache->empty;
      cache->empty = slab;
    } else {
      /* Too many empty slabs — return this page to the PMM.
       * Translate the slab's virtual address to physical, unmap
       * the page, and free it.  The virtual address is abandoned
       * (bump allocator cannot reclaim VAs) but 1 GiB of heap VA
       * is more than enough for Phase 2. */
      phys_addr_t phys = vmm_virt_to_phys((virt_addr_t)(uintptr_t)slab);
      if (phys != 0) {
        vmm_unmap_page((virt_addr_t)(uintptr_t)slab);
        pmm_free_pages(phys, 0);
      }
    }
  }

  cache->total_frees++;
  spinlock_unlock(&cache->lock);
}

slab_cache_t *slab_get_cache(const char *name) {
  for (uint32_t i = 0; i < g_cache_count; i++) {
    if (strcmp(g_caches[i].name, name) == 0) {
      return &g_caches[i];
    }
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Initialization — pre-defined caches                                       */
/* ═══════════════════════════════════════════════════════════════════════════
 */

void slab_init(void) {
  g_cache_count = 0;

  slab_cache_create("cap_token", 48, 8);
  slab_cache_create("microprogram", 512, 16);
  slab_cache_create("sched_node", 64, 8);
  slab_cache_create("graph_vertex", 128, 8);
  slab_cache_create("graph_edge", 64, 8);
  slab_cache_create("page_desc", 32, 8);

  serial_printf("  SLAB: created %u pre-defined caches\n", g_cache_count);
}
