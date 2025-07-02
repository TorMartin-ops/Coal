#pragma once
#ifndef PERCPU_ALLOC_H
#define PERCPU_ALLOC_H

#include <kernel/core/types.h>
#include <kernel/memory/slab.h> // Need slab_cache_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes per-CPU slab caches for small allocations.
 */
void percpu_kmalloc_init(void);

/**
 * @brief Allocates 'total_required_size' bytes from the per-CPU slab caches for 'cpu_id'.
 *
 * Attempts to find the best-fitting per-CPU slab cache for the given CPU and total size.
 * Returns the raw pointer allocated by the slab allocator.
 * The caller (kmalloc) is responsible for adding the header.
 *
 * @param total_required_size The total size needed (user size + header, aligned).
 * @param cpu_id The ID of the CPU requesting the allocation.
 * @param out_cache Optional output pointer to store the slab_cache_t* used.
 * @return Pointer to the raw allocated memory block (start of slab object), or NULL on failure.
 */
void *percpu_kmalloc(size_t total_required_size, int cpu_id, slab_cache_t **out_cache);

/**
 * @brief Frees memory previously allocated by percpu_kmalloc, using the cache pointer.
 *
 * Determines the correct per-CPU slab cache using the provided cache pointer
 * and calls the underlying slab_free.
 *
 * @param ptr Pointer to the raw memory block (start of slab object) to free.
 * @param cache Pointer to the slab_cache_t the object belongs to (from header).
 */
void percpu_kfree(void *ptr, slab_cache_t *cache);

/**
 * @brief Retrieves allocation statistics for a specific CPU's allocator. (Optional)
 */
int percpu_get_stats(int cpu_id, uint32_t *out_alloc_count, uint32_t *out_free_count);


#ifdef __cplusplus
}
#endif

#endif // PERCPU_ALLOC_H