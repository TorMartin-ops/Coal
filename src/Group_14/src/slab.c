#include "libc/stddef.h"    // Provides size_t, NULL
#include "libc/stdint.h"    // Provides uint32_t, etc.
#ifndef UINTPTR_MAX
typedef unsigned int uintptr_t;
#endif
#include "libc/stdbool.h"   // Provides bool, true, false

#include "terminal.h"       // Optional: for debug output
#include "slab.h"           // Slab allocator interface
#include "buddy.h"          // Uses buddy allocator to obtain new slabs

// Ensure PAGE_SIZE is defined (default to 4096 bytes if not provided).
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// -----------------------------------------------------------------------------
// Internal Data Structures
// -----------------------------------------------------------------------------

/**
 * @brief Slab header structure.
 *
 * Each slab is one page (PAGE_SIZE bytes) and begins with a header.
 * The header tracks the number of free objects in the slab and a pointer
 * to the free list of objects.
 */
typedef struct slab {
    struct slab *next;       // Pointer to the next slab in the cache.
    unsigned int free_count; // Number of free objects in this slab.
    void *free_list;         // Pointer to the first free object.
} slab_t;

/**
 * @brief Slab cache structure.
 *
 * The slab cache organizes multiple slabs for objects of a fixed size.
 * It maintains two linked lists: one for partially allocated slabs and one
 * for fully allocated slabs.
 */
struct slab_cache {
    const char *name;           // Descriptive name (for debugging)
    size_t obj_size;            // Size of each object in bytes
    unsigned int objs_per_slab; // Number of objects per slab (computed on first slab creation)
    slab_t *slab_partial;       // List of slabs with free objects.
    slab_t *slab_full;          // List of slabs that are fully allocated.
};

// -----------------------------------------------------------------------------
// Internal Helper Functions
// -----------------------------------------------------------------------------

/**
 * @brief Allocates and initializes a new slab for the given cache.
 *
 * Uses buddy_alloc() to allocate one page (PAGE_SIZE) and subdivides it into objects.
 * The slab header is stored at the beginning of the page. The remaining space is divided
 * into fixed–size objects that are chained into a free list.
 *
 * @param cache The slab cache for which a new slab is needed.
 * @return Pointer to the new slab header, or NULL on failure.
 */
static slab_t *slab_create_new(slab_cache_t *cache) {
    // Allocate one page for the new slab using the buddy allocator.
    void *page = buddy_alloc(PAGE_SIZE);
    if (!page) {
        terminal_write("slab_create_new: Failed to allocate new slab.\n");
        return NULL;
    }
    slab_t *slab = (slab_t *)page;
    slab->next = NULL;
    size_t header_size = sizeof(slab_t);
    size_t available = PAGE_SIZE - header_size;
    
    // Compute how many objects fit in this slab.
    cache->objs_per_slab = available / cache->obj_size;
    slab->free_count = cache->objs_per_slab;
    
    // Build the free list: objects are laid out sequentially after the header.
    uint8_t *obj_ptr = (uint8_t *)page + header_size;
    void **free_list = NULL;
    for (unsigned int i = 0; i < cache->objs_per_slab; i++) {
        void *current_obj = (void *)(obj_ptr + i * cache->obj_size);
        if (i < cache->objs_per_slab - 1)
            *(void **)current_obj = (void *)(obj_ptr + (i + 1) * cache->obj_size);
        else
            *(void **)current_obj = NULL;
        if (i == 0)
            free_list = (void **)current_obj;
    }
    slab->free_list = free_list;
    
    return slab;
}

// -----------------------------------------------------------------------------
// Public API Functions
// -----------------------------------------------------------------------------

slab_cache_t *slab_create(const char *name, size_t obj_size) {
    if (obj_size == 0) {
        terminal_write("slab_create: Object size must be > 0\n");
        return NULL;
    }
    // Allocate the cache structure using the buddy allocator.
    slab_cache_t *cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
    if (!cache) {
        terminal_write("slab_create: Failed to allocate cache structure.\n");
        return NULL;
    }
    cache->name = name;
    cache->obj_size = obj_size;
    cache->objs_per_slab = 0; // Will be computed when the first slab is created.
    cache->slab_partial = NULL;
    cache->slab_full = NULL;
    return cache;
}

void *slab_alloc(slab_cache_t *cache) {
    if (!cache)
        return NULL;
    
    slab_t *slab = cache->slab_partial;
    // If no slab with free objects exists, allocate a new slab.
    if (!slab) {
        slab = slab_create_new(cache);
        if (!slab)
            return NULL;
        // Insert the new slab into the partial list.
        slab->next = cache->slab_partial;
        cache->slab_partial = slab;
    }
    
    // Allocate an object from the slab.
    void *obj = slab->free_list;
    if (!obj)
        return NULL;  // Should not occur if free_count > 0.
    
    // Update the free list and decrement the free count.
    slab->free_list = *(void **)obj;
    slab->free_count--;
    
    // If the slab becomes fully allocated, move it to the full list.
    if (slab->free_count == 0) {
        cache->slab_partial = slab->next;
        slab->next = cache->slab_full;
        cache->slab_full = slab;
    }
    
    return obj;
}

void slab_free(slab_cache_t *cache, void *obj) {
    if (!cache || !obj)
        return;
    
    // Compute the base address of the slab (assuming slabs are PAGE_SIZE aligned).
    uintptr_t obj_addr = (uintptr_t)obj;
    uintptr_t slab_base = obj_addr & ~(PAGE_SIZE - 1);
    slab_t *slab = (slab_t *)slab_base;
    
    // Return the object to the slab's free list.
    *(void **)obj = slab->free_list;
    slab->free_list = obj;
    slab->free_count++;
    
    // If the slab was in the full list and now has one free object, move it to the partial list.
    if (slab->free_count == 1) {
        slab_t **prev = &cache->slab_full;
        slab_t *curr = cache->slab_full;
        while (curr) {
            if (curr == slab) {
                *prev = curr->next;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }
        slab->next = cache->slab_partial;
        cache->slab_partial = slab;
    }
    
    // Optional: If the slab becomes completely free (free_count equals objs_per_slab),
    // you may consider returning it to the buddy allocator.
}

void slab_destroy(slab_cache_t *cache) {
    if (!cache)
        return;
    
    // Free all slabs in the partial list.
    slab_t *slab = cache->slab_partial;
    while (slab) {
        slab_t *next = slab->next;
        buddy_free(slab, PAGE_SIZE);
        slab = next;
    }
    
    // Free all slabs in the full list.
    slab = cache->slab_full;
    while (slab) {
        slab_t *next = slab->next;
        buddy_free(slab, PAGE_SIZE);
        slab = next;
    }
    
    // Free the cache structure itself.
    buddy_free(cache, sizeof(slab_cache_t));
}
