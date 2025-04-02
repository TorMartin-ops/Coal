#ifndef SLAB_H
#define SLAB_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle for the slab cache structure.
 */
typedef struct slab_cache slab_cache_t;

/**
 * slab_create
 *
 * Creates a new slab cache for objects of 'obj_size' bytes. 
 * Allocates the cache structure from buddy (or your base allocator).
 *
 * @param name     debug name for the cache
 * @param obj_size size of each object in bytes
 * @return pointer to slab_cache_t, or NULL on failure
 */
slab_cache_t *slab_create(const char *name, size_t obj_size);

/**
 * slab_alloc
 *
 * Allocates one object from the given slab cache.
 * 
 * @param cache the slab cache
 * @return pointer to allocated object, or NULL
 */
void *slab_alloc(slab_cache_t *cache);

/**
 * slab_free
 *
 * Frees the object 'obj' back into the slab cache.
 *
 * @param cache the slab cache
 * @param obj   pointer to an object previously allocated from this cache
 */
void slab_free(slab_cache_t *cache, void *obj);

/**
 * slab_destroy
 *
 * Frees all slabs in the cache, then frees the cache structure itself.
 */
void slab_destroy(slab_cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif // SLAB_H
