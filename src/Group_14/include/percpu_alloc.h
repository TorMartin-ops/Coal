#pragma once
#ifndef PERCPU_ALLOC_H
#define PERCPU_ALLOC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes per-CPU slab caches for small allocations.
 *
 * Creates slab caches for predefined size classes (up to 4096 bytes typically)
 * for each CPU detected (up to MAX_CPUS). Must be called once during system startup
 * after the slab allocator itself is ready.
 */
void percpu_kmalloc_init(void);

/**
 * @brief Allocates 'size' bytes from the per-CPU slab caches for 'cpu_id'.
 *
 * IMPORTANT: This function assumes 'size' is within the range handled by
 * the slab caches (e.g., <= 4096 bytes) and 'cpu_id' is valid.
 * It attempts to allocate from the appropriate slab cache for the given CPU.
 * Fallback to other allocators (like buddy) should be handled by the caller
 * (e.g., the main kmalloc function) if this function returns NULL.
 *
 * @param size The requested size in bytes (assumed <= SMALL_ALLOC_MAX).
 * @param cpu_id The ID of the CPU requesting the allocation (assumed valid).
 * @return Pointer to the allocated object, or NULL on failure (e.g., slab cache full).
 */
void *percpu_kmalloc(size_t size, int cpu_id);

/**
 * @brief Frees memory previously allocated by percpu_kmalloc.
 *
 * IMPORTANT: This function assumes the memory pointed to by 'ptr' was originally
 * allocated via percpu_kmalloc with the same 'size' and 'cpu_id', and that 'size'
 * corresponds to a size handled by the per-CPU slab caches.
 *
 * @param ptr Pointer to the memory block to free.
 * @param size The original size requested during allocation (must match).
 * @param cpu_id The ID of the CPU that performed the original allocation.
 */
void percpu_kfree(void *ptr, size_t size, int cpu_id);

/**
 * @brief Retrieves allocation statistics for a specific CPU's allocator. (Optional)
 *
 * @param cpu_id The CPU identifier.
 * @param out_alloc_count Pointer to store the allocation count (can be NULL).
 * @param out_free_count Pointer to store the free count (can be NULL).
 * @return 0 on success, -1 if cpu_id is invalid.
 */
int percpu_get_stats(int cpu_id, uint32_t *out_alloc_count, uint32_t *out_free_count);


#ifdef __cplusplus
}
#endif

#endif // PERCPU_ALLOC_H