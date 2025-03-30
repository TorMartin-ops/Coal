#pragma once
#ifndef PERCPU_ALLOC_H
#define PERCPU_ALLOC_H

#include "libc/stddef.h"   // Provides NULL
#include "libc/stdint.h"   // Provides size_t, uint32_t, etc.
#include "libc/stdbool.h"  // Provides bool, true, false

// If uintptr_t is not defined, define it here for 32-bit systems.
#ifndef UINTPTR_MAX
typedef unsigned int uintptr_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the per-CPU kernel allocator.
 *
 * This function sets up per-CPU slab caches for small allocations.
 * It must be called during system initialization after paging and the buddy allocator are ready.
 */
void percpu_kmalloc_init(void);

/**
 * @brief Allocates memory from the per-CPU allocator.
 *
 * For requests up to SMALL_ALLOC_MAX bytes, this function rounds the allocation up
 * to the next supported size class and uses the corresponding per-CPU slab cache.
 * Larger requests are handled by the buddy allocator.
 *
 * @param size   The number of bytes requested.
 * @param cpu_id The CPU identifier (0 to MAX_CPUS - 1).
 * @return A pointer to the allocated memory, or NULL on failure.
 */
void *percpu_kmalloc(size_t size, int cpu_id);

/**
 * @brief Frees memory previously allocated by percpu_kmalloc.
 *
 * The caller must supply the original allocation size so that the allocator
 * can determine the correct slab cache or buddy free routine.
 *
 * @param ptr    Pointer to the allocated memory.
 * @param size   The original allocation size.
 * @param cpu_id The CPU identifier (0 to MAX_CPUS - 1) that performed the allocation.
 */
void percpu_kfree(void *ptr, size_t size, int cpu_id);

#ifdef __cplusplus
}
#endif

#endif // PERCPU_ALLOC_H
