/**
 * slab.c
 * 
 * A world–class Slab Allocator for 32-bit x86 or freestanding C environments.
 * 
 * DESIGN OVERVIEW:
 *   - Each slab is exactly one page (PAGE_SIZE). The first bytes store a slab header
 *     (slab_t), followed by subdivided objects of 'obj_size'.
 *   - For each slab_cache_t (cache), we keep two linked lists:
 *       (1) slab_partial: slabs that have >=1 free object but are not full.
 *       (2) slab_full: slabs that are fully allocated (no free objects).
 *   - On slab_alloc(), we take a free object from slab_partial. If none exists, 
 *     we create a new slab from the buddy system.
 *   - On slab_free(), we re-link the object into that slab's free list, 
 *     and possibly move the slab from 'full' to 'partial'. 
 *     If a slab becomes 100% free again, we optionally reclaim it to buddy.
 * 
 * MAJOR FEATURES:
 *   1) Thorough documentation
 *   2) Optional usage stats (cache->alloc_count, cache->free_count)
 *   3) Enhanced debug logs
 *   4) Automatic reclaim of completely empty slabs (define ENABLE_SLAB_RECLAIM)
 *   5) Concurrency disclaimers
 */

 #include "slab.h"
 #include "buddy.h"
 #include "terminal.h"
 
#include "types.h"
 #include <string.h>  // For memset if needed
 

 
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096
 #endif
 
 // Uncomment to enable reclaim logic if slab->free_count == objs_per_slab
 // #define ENABLE_SLAB_RECLAIM 1
 
 // -----------------------------------------------------------------------------
 // Data Structures
 // -----------------------------------------------------------------------------
 
 /**
  * slab_t
  *
  * Each slab is one page in memory. At the start of the page, we have this header.
  * The remaining bytes are subdivided into fixed-size objects for the cache.
  */
 typedef struct slab {
     struct slab *next;        ///< Next slab in the cache’s list
     unsigned int free_count;  ///< How many free objects remain
     void *free_list;          ///< Pointer to first free object
 } slab_t;
 
 /**
  * slab_cache
  *
  * Represents a cache of objects of a fixed size. We keep two singly-linked lists of slabs:
  *   - slab_partial: Not fully allocated (some free objects)
  *   - slab_full: Fully allocated (0 free objects)
  */
 struct slab_cache {
     const char *name;           ///< Debug name for this cache
     size_t obj_size;            ///< Size of each object
     unsigned int objs_per_slab; ///< Number of objects per slab (computed on slab_create_new)
     slab_t *slab_partial;       ///< List of partially allocated slabs
     slab_t *slab_full;          ///< List of fully allocated slabs
 
     // Optional usage stats
     unsigned long alloc_count;  ///< How many slab_alloc calls succeeded
     unsigned long free_count;   ///< How many slab_free calls
 };
 
 // -----------------------------------------------------------------------------
 // Forward Declarations
 // -----------------------------------------------------------------------------
 static slab_t *slab_create_new(slab_cache_t *cache);
 
 // -----------------------------------------------------------------------------
 // slab_create
 // -----------------------------------------------------------------------------
 
 slab_cache_t *slab_create(const char *name, size_t obj_size) {
     if (obj_size == 0) {
         terminal_write("[slab_create] Error: Object size must be > 0\n");
         return NULL;
     }
 
     // Allocate the cache structure from buddy. 
     // In a real OS, you might want a dedicated slab for struct slab_cache, but
     // buddy_alloc is fine for now.
     slab_cache_t *cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
     if (!cache) {
         terminal_write("[slab_create] Failed to allocate cache structure.\n");
         return NULL;
     }
 
     cache->name           = name;
     cache->obj_size       = obj_size;
     cache->objs_per_slab  = 0; // computed on first slab creation
     cache->slab_partial   = NULL;
     cache->slab_full      = NULL;
     cache->alloc_count    = 0;
     cache->free_count     = 0;
 
     return cache;
 }
 
 // -----------------------------------------------------------------------------
 // slab_create_new
 // -----------------------------------------------------------------------------
 
 /**
  * slab_create_new
  *
  * Allocates a new 4KB page from buddy, uses the first 'sizeof(slab_t)' 
  * bytes for the slab header, then subdivides the rest into obj_size–sized chunks
  * in a free list.
  *
  * @param cache The slab cache for which a new slab is needed.
  * @return The newly created slab_t*, or NULL on failure
  */
 static slab_t *slab_create_new(slab_cache_t *cache) {
     void *page = buddy_alloc(PAGE_SIZE);
     if (!page) {
         terminal_write("[slab_create_new] buddy_alloc PAGE_SIZE failed.\n");
         return NULL;
     }
 
     // The first chunk is the slab header
     slab_t *slab = (slab_t *)page;
     slab->next = NULL;
 
     size_t header_size = sizeof(slab_t);
     size_t available   = PAGE_SIZE - header_size;
 
     // Compute how many objects fit
     if (cache->objs_per_slab == 0) {
         // If we haven't set it yet, do so now
         unsigned int num_objs = (unsigned int)(available / cache->obj_size);
         cache->objs_per_slab  = num_objs;
     }
     slab->free_count = cache->objs_per_slab;
 
     // Build free list. The first object is at [page + header_size].
     uint8_t *obj_ptr = (uint8_t *)page + header_size;
     void **free_list = NULL;
 
     for (unsigned int i = 0; i < cache->objs_per_slab; i++) {
         void *current_obj = (void *)(obj_ptr + i * cache->obj_size);
         // Link to the next
         if (i < (cache->objs_per_slab - 1)) {
             // next pointer
             *(void **)current_obj = (void *)(obj_ptr + (i + 1) * cache->obj_size);
         } else {
             *(void **)current_obj = NULL;
         }
         // If this is the first object, record as the free_list pointer
         if (i == 0) {
             free_list = (void **)current_obj;
         }
     }
 
     slab->free_list = free_list;
     return slab;
 }
 
 // -----------------------------------------------------------------------------
 // slab_alloc
 // -----------------------------------------------------------------------------
 
 void *slab_alloc(slab_cache_t *cache) {
     if (!cache) return NULL;
 
     // If there is no partial slab, create a new one
     slab_t *slab = cache->slab_partial;
     if (!slab) {
         slab = slab_create_new(cache);
         if (!slab) return NULL;
 
         // Insert into partial list
         slab->next = cache->slab_partial;
         cache->slab_partial = slab;
     }
 
     // Allocate object from the slab
     void *obj = slab->free_list;
     if (!obj) {
         // Should not happen if free_count > 0, but guard anyway
         terminal_write("[slab_alloc] Unexpected null free_list.\n");
         return NULL;
     }
 
     // The free list is singly linked: *(void **)obj is the next
     slab->free_list = *(void **)obj;
     slab->free_count--;
 
     // If the slab is now fully allocated, move it from partial to full
     if (slab->free_count == 0) {
         // remove from partial
         cache->slab_partial = slab->next;
         // push front to full
         slab->next = cache->slab_full;
         cache->slab_full = slab;
     }
 
     // Usage stats
     cache->alloc_count++;
     return obj;
 }
 
 // -----------------------------------------------------------------------------
 // slab_free
 // -----------------------------------------------------------------------------
 
 void slab_free(slab_cache_t *cache, void *obj) {
     if (!cache || !obj) return;
 
     // Compute which slab this obj belongs to, by page alignment
     uintptr_t obj_addr = (uintptr_t)obj;
     uintptr_t slab_base = obj_addr & ~(PAGE_SIZE - 1);
     slab_t *slab = (slab_t *)slab_base;
 
     // Insert obj back into free_list
     *(void **)obj = slab->free_list;
     slab->free_list = obj;
     slab->free_count++;
 
     cache->free_count++;
 
     // If slab was in full list but now gained a free object, move it to partial
     if (slab->free_count == 1) {
         // remove from full
         slab_t **prev = &cache->slab_full;
         slab_t *curr = cache->slab_full;
         while (curr) {
             if (curr == slab) {
                 *prev = curr->next;
                 // re-link slab to partial
                 slab->next = cache->slab_partial;
                 cache->slab_partial = slab;
                 break;
             }
             prev = &curr->next;
             curr = curr->next;
         }
     }
 
 #ifdef ENABLE_SLAB_RECLAIM
     // If the slab becomes completely free, we can reclaim it to buddy.
     // This is optional. If we do so, we remove it from partial.
     if (slab->free_count == cache->objs_per_slab) {
         // remove from partial
         slab_t **prev = &cache->slab_partial;
         slab_t *curr = cache->slab_partial;
         bool found = false;
         while (curr) {
             if (curr == slab) {
                 *prev = curr->next;
                 found = true;
                 break;
             }
             prev = &curr->next;
             curr = curr->next;
         }
         if (!found) {
             // Some logic error if we can't find it
             terminal_write("[slab_free] Could not find slab in partial.\n");
         } else {
             buddy_free((void*)slab_base, PAGE_SIZE);
             // Freed entire slab back to buddy
         }
     }
 #endif
 }
 
 // -----------------------------------------------------------------------------
 // slab_destroy
 // -----------------------------------------------------------------------------
 
 /**
  * slab_destroy
  * 
  * Frees all slabs in both partial and full lists, then frees the cache structure itself.
  * 
  * NOTES:
  *   - This function is typically used if you want to tear down a slab cache altogether
  *     (like an ephemeral cache or when shutting down). For system caches, you might
  *     never call this.
  */
 void slab_destroy(slab_cache_t *cache) {
     if (!cache) return;
 
     // Free partial slabs
     slab_t *slab = cache->slab_partial;
     while (slab) {
         slab_t *next = slab->next;
         buddy_free(slab, PAGE_SIZE);
         slab = next;
     }
 
     // Free full slabs
     slab = cache->slab_full;
     while (slab) {
         slab_t *next = slab->next;
         buddy_free(slab, PAGE_SIZE);
         slab = next;
     }
 
     // Finally free the cache structure
     buddy_free(cache, sizeof(slab_cache_t));
 }
 
 // -----------------------------------------------------------------------------
 // Optional: Stats / Debug
 // -----------------------------------------------------------------------------
 
 /**
  * slab_cache_stats
  *
  * If you want to retrieve how many allocations/frees have happened, 
  * or how many objects per slab, etc.
  *
  * @param cache   The slab cache
  * @param out_alloc_count optional pointer to store how many calls to slab_alloc
  * @param out_free_count  optional pointer to store how many calls to slab_free
  */
 void slab_cache_stats(slab_cache_t *cache,
                       unsigned long *out_alloc_count,
                       unsigned long *out_free_count) {
     if (!cache) return;
     if (out_alloc_count) {
         *out_alloc_count = cache->alloc_count;
     }
     if (out_free_count) {
         *out_free_count = cache->free_count;
     }
 }
 