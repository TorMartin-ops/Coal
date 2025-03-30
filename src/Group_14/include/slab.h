#pragma once
#ifndef SLAB_H
#define SLAB_H

#include "libc/stddef.h"   // Provides size_t, NULL
#include "libc/stdint.h"   // Provides uint32_t, etc.
#include "libc/stdbool.h"  // Provides bool, true, false

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque slab cache structure.
 *
 * A slab cache manages fixed–size objects for a particular object size.
 */
typedef struct slab_cache slab_cache_t;

/**
 * @brief Creates a slab cache for objects of a specified size.
 *
 * Each new slab is allocated from the buddy allocator (typically one page)
 * and subdivided into fixed–size objects. The cache maintains two lists:
 * one for slabs with available objects and one for slabs that are full.
 *
 * @param name     A human-readable name for the cache (for debugging).
 * @param obj_size The size in bytes of each object (must be > 0).
 * @return A pointer to the new slab cache, or NULL on failure.
 */
slab_cache_t *slab_create(const char *name, size_t obj_size);

/**
 * @brief Allocates an object from the given slab cache.
 *
 * If no slab with free objects exists, a new slab is allocated.
 *
 * @param cache The slab cache.
 * @return A pointer to the allocated object, or NULL on failure.
 */
void *slab_alloc(slab_cache_t *cache);

/**
 * @brief Frees an object back to the slab cache.
 *
 * The object must have been allocated from the provided cache.
 *
 * @param cache The slab cache.
 * @param obj   The object to free.
 */
void slab_free(slab_cache_t *cache, void *obj);

/**
 * @brief Destroys the slab cache, freeing all slabs and the cache structure.
 *
 * @param cache The slab cache to destroy.
 */
void slab_destroy(slab_cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif // SLAB_H
