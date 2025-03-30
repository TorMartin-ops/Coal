#pragma once
#ifndef KMALLOC_H
#define KMALLOC_H

#include "libc/stddef.h"   // Provides size_t, NULL
#include "libc/stdint.h"   // Provides uint32_t, etc.
#include "libc/stdbool.h"  // Provides bool, true, false

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the unified kernel memory allocator.
 *
 * This function must be called during kernel initialization (after paging and the buddy allocator are initialized)
 * to set up slab caches for small allocations.
 */
void kmalloc_init(void);

/**
 * @brief Allocates memory from the kernel.
 *
 * For requests up to SMALL_ALLOC_MAX bytes, the allocation is rounded up to the next supported size class
 * and handled by the slab allocator; larger requests are handled by the buddy allocator.
 *
 * @param size The number of bytes requested.
 * @return A pointer to the allocated memory, or NULL on failure.
 */
void *kmalloc(size_t size);

/**
 * @brief Frees memory previously allocated by kmalloc.
 *
 * The caller must supply the original allocation size so that the allocator can choose the proper free routine.
 *
 * @param ptr  Pointer to the memory to free.
 * @param size The original allocation size.
 */
void kfree(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif // KMALLOC_H
