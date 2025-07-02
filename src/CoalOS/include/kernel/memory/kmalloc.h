#pragma once
#ifndef KMALLOC_H
#define KMALLOC_H

#include <kernel/core/types.h> // Includes size_t, uint32_t etc.

// Configuration Macro: Define this in your build system (e.g., CMakeLists.txt)
// if you want to enable the per-CPU allocation strategy. Otherwise, it uses
// the global slab allocator strategy.
// Example CMake: target_compile_definitions(mykernel PRIVATE USE_PERCPU_ALLOC)
// #define USE_PERCPU_ALLOC // <<--- We will effectively enable this in kmalloc.c

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the primary kernel memory allocator.
 *
 * Must be called once during kernel startup after the underlying allocators
 * (buddy, slab/percpu) are ready. Initializes internal structures and slab caches.
 */
void kmalloc_init(void);

/**
 * @brief Allocates a block of memory of at least 'user_size' bytes.
 *
 * This is the main entry point for kernel dynamic memory allocation.
 * It automatically selects an appropriate underlying allocator (slab or buddy)
 * based on the requested size and configuration.
 * It stores metadata before the returned pointer for robust freeing.
 *
 * @param user_size The minimum number of bytes required by the caller.
 * @return Pointer to the allocated memory block (aligned appropriately),
 * or NULL if allocation fails. The pointer points to the user data area,
 * after the internal metadata header.
 */
void *kmalloc(size_t user_size);

/**
 * @brief Frees a block of memory previously allocated by kmalloc.
 *
 * This function automatically determines the correct underlying allocator
 * (slab or buddy) and the original allocated size using metadata stored
 * before the given pointer.
 *
 * @param ptr Pointer to the user data area of the memory block to free
 * (the pointer originally returned by kmalloc).
 * If NULL, the function does nothing.
 */
void kfree(void *ptr);

/**
 * @brief Retrieves global allocation statistics (Global Slab Mode Only).
 *
 * If using the global slab allocator mode (USE_PERCPU_ALLOC not defined),
 * this function retrieves the total number of successful slab allocations
 * and frees performed via kmalloc.
 * Note: This does not track buddy allocator stats directly via kmalloc, nor
 * per-CPU stats if USE_PERCPU_ALLOC is defined. Use percpu_get_stats for that.
 *
 * @param out_alloc Pointer to store the total slab allocation count (can be NULL).
 * @param out_free Pointer to store the total slab free count (can be NULL).
 */
void kmalloc_get_global_stats(uint32_t *out_alloc, uint32_t *out_free);

/**
 * @brief Allocates multiple memory blocks of the same size efficiently.
 *
 * This function attempts to allocate multiple objects from the same slab cache
 * to improve cache locality and reduce allocation overhead. This is particularly
 * beneficial when allocating many objects of the same type (e.g., PCBs, file descriptors).
 *
 * @param size Size of each individual object to allocate.
 * @param count Number of objects to allocate.
 * @param ptrs Array to store pointers to allocated objects (must have space for 'count' pointers).
 * @return Pointer to first allocated object on success, NULL if any allocation fails.
 *         On partial failure, all successfully allocated objects are automatically freed.
 */
void *kmalloc_bulk(size_t size, size_t count, void **ptrs);


#ifdef __cplusplus
}
#endif

#endif // KMALLOC_H