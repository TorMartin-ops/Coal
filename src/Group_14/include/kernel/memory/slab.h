#ifndef SLAB_H
#define SLAB_H

#include <kernel/core/types.h> // Includes size_t, bool,stdint.h, etc.
#include <kernel/sync/spinlock.h> // Include spinlock header

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration for slab_t used internally by slab.c
typedef struct slab slab_t;

/**
 * @brief Structure representing a slab cache.
 */
typedef struct slab_cache {
    const char *name;           // Debug name for the cache.
    size_t user_obj_size;       // Size requested by the user.
    size_t internal_slot_size;  // Actual size allocated per object (user + metadata like footer).
    size_t alignment;           // Alignment required for objects in this cache.
    unsigned int objs_per_slab_max; // Max possible objects per slab (calculated once).

    // Linked lists of slabs associated with this cache.
    slab_t *slab_partial;       // List of partially filled slabs.
    slab_t *slab_full;          // List of completely full slabs.
    slab_t *slab_empty;         // List of completely empty slabs.

    // Slab Coloring Info
    unsigned int color_next;    // Next color offset to use for a new slab.
    unsigned int color_range;   // Range of color offsets (e.g., cache line size).

    // Statistics
    unsigned long alloc_count;  // Total objects allocated.
    unsigned long free_count;   // Total objects freed back.

    // Concurrency Control
    spinlock_t lock;            // Spinlock to protect cache metadata and lists.

    // Optional: Constructor/Destructor function pointers
    void (*constructor)(void *obj);
    void (*destructor)(void *obj);

} slab_cache_t;


/**
 * slab_create
 *
 * Creates a new slab cache.
 * Calculates internal object size including metadata (like footers).
 * Initializes coloring parameters and the cache lock.
 *
 * @param name     Debug name for the cache (should be persistent).
 * @param obj_size Size of each object requested by the user.
 * @param align    Required alignment for objects (power of 2), or 0 for default.
 * @param color_range Range for slab coloring offset (e.g., 64 for L1 cache line), 0 to disable.
 * @param constructor Optional constructor function (can be NULL).
 * @param destructor Optional destructor function (can be NULL).
 * @return Pointer to the created slab_cache_t, or NULL on failure.
 */
slab_cache_t *slab_create(const char *name, size_t obj_size, size_t align,
                          unsigned int color_range,
                          void (*constructor)(void*), void (*destructor)(void*));

/**
 * slab_alloc
 *
 * Allocates one object from the cache. Writes metadata (e.g., footer canary).
 * Calls constructor if provided. Thread-safe via internal locking.
 *
 * @param cache Pointer to the slab cache.
 * @return Pointer to the allocated object (start of user area), or NULL on failure.
 */
void *slab_alloc(slab_cache_t *cache);

/**
 * slab_free
 *
 * Frees an object back into its slab cache. Checks metadata (e.g., footer canary).
 * Calls destructor if provided. Thread-safe via internal locking.
 *
 * @param cache Pointer to the slab cache (recommended, but can be NULL if metadata reliable).
 * @param obj   Pointer to the object (start of user area) previously allocated.
 */
void slab_free(slab_cache_t *cache, void *obj);

/**
 * slab_destroy
 *
 * Destroys a slab cache, freeing all associated slab pages and the descriptor.
 * Thread-safe via internal locking. Ensure no objects are in use before calling.
 *
 * @param cache Pointer to the slab cache to destroy.
 */
void slab_destroy(slab_cache_t *cache);

/**
 * slab_cache_stats
 *
 * Retrieves allocation and free counts. Thread-safe via internal locking.
 *
 * @param cache      Pointer to the slab cache.
 * @param out_alloc  Pointer to store the total allocation count (can be NULL).
 * @param out_free   Pointer to store the total free count (can be NULL).
 */
void slab_cache_stats(slab_cache_t *cache, unsigned long *out_alloc, unsigned long *out_free);


#ifdef __cplusplus
}
#endif

#endif // SLAB_H