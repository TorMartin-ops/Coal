/**
 * @file slab.h
 * @brief Slab allocator for small fixed-size objects.
 *
 * A slab is one page: a header followed by equally sized objects. The cache
 * allocates from its current slab; other slabs with free objects sit on the
 * partial list, full slabs on no list until one of their objects is freed. Objects need no per-object
 * header: the slab (and with it the cache) is found by masking the pointer.
 */
#ifndef SLAB_H
#define SLAB_H

#include <kernel/core/types.h>
#include <kernel/sync/spinlock.h>

struct slab;

typedef struct slab_cache {
    size_t obj_size;            /* multiple of 16 */
    uint32_t limit;             /* highest page offset an object can start at */
    struct slab *cur;           /* allocation target */
    struct slab *partial;
    spinlock_t lock;
} slab_cache_t;

/** Prepare a cache for objects of obj_size bytes (16 <= obj_size <= 1024, multiple of 16). */
void slab_cache_init(slab_cache_t *cache, size_t obj_size);

/** Allocate one object; NULL when out of memory. */
void *slab_alloc(slab_cache_t *cache);

/** Return an object to the cache that allocated it. */
void slab_free(void *obj);

#endif /* SLAB_H */
