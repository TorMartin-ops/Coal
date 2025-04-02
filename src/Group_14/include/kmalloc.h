#pragma once
#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

// Configuration Macro: Define this in your build system (e.g., CMakeLists.txt)
// if you want to enable the per-CPU allocation strategy. Otherwise, it uses
// the global slab allocator strategy.
// Example CMake: target_compile_definitions(mykernel PRIVATE USE_PERCPU_ALLOC)
// #define USE_PERCPU_ALLOC

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the primary kernel memory allocator.
 *
 * Depending on whether USE_PERCPU_ALLOC is defined, this initializes either
 * the per-CPU slab caches or the global slab caches. Must be called once
 * during kernel startup after the underlying allocators (buddy, slab) are ready.
 */
void kmalloc_init(void);

/**
 * @brief Allocates a block of memory of at least 'size' bytes.
 *
 * This is the main entry point for kernel dynamic memory allocation.
 * It employs different strategies based on the requested size and configuration:
 * - If USE_PERCPU_ALLOC is defined:
 * - For small sizes (<= SMALL_ALLOC_MAX): Attempts allocation from the current CPU's slab cache.
 * Falls back to the buddy allocator if the per-CPU slab allocation fails.
 * - For large sizes (> SMALL_ALLOC_MAX): Uses the buddy allocator directly.
 * - If USE_PERCPU_ALLOC is not defined:
 * - For small sizes (<= SMALL_ALLOC_MAX): Attempts allocation from a global slab cache.
 * Falls back to the buddy allocator if slab allocation fails.
 * - For large sizes (> SMALL_ALLOC_MAX): Uses the buddy allocator directly.
 *
 * @param size The minimum number of bytes to allocate.
 * @return Pointer to the allocated memory block, or NULL if allocation fails.
 */
void *kmalloc(size_t size);

/**
 * @brief Frees a block of memory previously allocated by kmalloc.
 *
 * The freeing mechanism depends on the original allocation size and configuration:
 * - If USE_PERCPU_ALLOC is defined:
 * - Small sizes are freed back to the appropriate per-CPU slab cache.
 * - Large sizes are freed back to the buddy allocator.
 * - If USE_PERCPU_ALLOC is not defined:
 * - Small sizes are freed back to the appropriate global slab cache.
 * - Large sizes are freed back to the buddy allocator.
 *
 * IMPORTANT: The 'size' parameter must match the original size passed to kmalloc
 * when the block was allocated. Incorrect size can lead to corruption.
 *
 * @param ptr Pointer to the memory block to free. If NULL, the function does nothing.
 * @param size The original size of the memory block requested during allocation.
 */
void kfree(void *ptr, size_t size);

/**
 * @brief Retrieves global allocation statistics. (Optional)
 *
 * If using the global slab allocator mode (USE_PERCPU_ALLOC not defined),
 * this function can retrieve the total number of successful allocations
 * and frees performed via the slab caches.
 * Note: This does not track per-CPU stats or buddy allocator stats.
 *
 * @param out_alloc Pointer to store the total allocation count (can be NULL).
 * @param out_free Pointer to store the total free count (can be NULL).
 */
void kmalloc_get_global_stats(uint32_t *out_alloc, uint32_t *out_free);


#ifdef __cplusplus
}
#endif

#endif // KMALLOC_H