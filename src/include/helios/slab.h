/**
 * @file slab.h
 * @brief Helios OS — Slab allocator for fixed-size kernel objects.
 *
 * Each slab is one physical page (4 KiB) mapped into KERNEL_HEAP_BASE.
 * Objects are allocated from named caches with configurable size/alignment.
 */

#ifndef HELIOS_SLAB_H
#define HELIOS_SLAB_H

#include <helios/types.h>
#include <helios/spinlock.h>

/* Forward declaration */
typedef struct slab slab_t;

/**
 * @brief A slab page header, stored at the start of each slab page.
 */
struct slab {
    struct slab *next;
    uint32_t     free_count;
    uint32_t     obj_count;
    uint8_t     *free_bitmap;   /* Points into this slab's own data area */
    uint8_t      data[];        /* Objects follow */
};

/**
 * @brief A slab cache manages a pool of fixed-size objects.
 */
typedef struct slab_cache {
    const char     *name;
    size_t          obj_size;
    size_t          alignment;
    spinlock_t      lock;
    slab_t         *partial;    /* Slabs with free objects */
    slab_t         *full;       /* Fully allocated slabs */
    slab_t         *empty;      /* Fully free slabs (cached, up to 2) */
    uint64_t        total_allocs;
    uint64_t        total_frees;
} slab_cache_t;

/**
 * @brief Initialize the slab allocator and create pre-defined caches.
 *
 * Must be called after vmm_init_sasos().
 */
void slab_init(void);

/**
 * @brief Create a new slab cache.
 *
 * @param name       Human-readable cache name (e.g. "cap_token").
 * @param obj_size   Size of each object in bytes.
 * @param alignment  Alignment requirement for objects.
 * @return Pointer to the new cache, or NULL on failure.
 */
slab_cache_t *slab_cache_create(const char *name, size_t obj_size,
                                 size_t alignment);

/**
 * @brief Allocate an object from a slab cache.
 *
 * @param cache  The cache to allocate from.
 * @return Pointer to the allocated object, or NULL if OOM.
 */
void *slab_alloc(slab_cache_t *cache);

/**
 * @brief Free an object back to its slab cache.
 *
 * @param cache  The cache the object belongs to.
 * @param obj    Pointer to the object to free.
 */
void slab_free(slab_cache_t *cache, void *obj);

/**
 * @brief Look up a slab cache by name.
 *
 * @param name  Cache name to search for.
 * @return Pointer to the cache, or NULL if not found.
 */
slab_cache_t *slab_get_cache(const char *name);

#endif /* HELIOS_SLAB_H */
