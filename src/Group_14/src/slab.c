/**
 * slab.c - Debug Instrumented Slab Allocator (Revised)
 */

 #include "slab.h"         // Now includes the definition of slab_cache_t
 #include "buddy.h"
 #include "terminal.h"     // Make sure terminal_printf is available
 #include "types.h"
 #include <string.h>      // For memset
 
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096
 #endif
 
 // Define a magic number to help detect slab header corruption
 #define SLAB_MAGIC 0x51AB51AB
 
 // Define alignment requirement (usually pointer size is sufficient)
 #define SLAB_HEADER_ALIGNMENT sizeof(void*)
 
 // Helper macro for alignment calculation
 // Ensure uintptr_t is defined in types.h or include <stdint.h> if available
 #define ALIGN_UP(addr, align) (((uintptr_t)(addr) + (align) - 1) & ~((uintptr_t)(align) - 1))
 
 
 // Optional Slab Reclaim Feature Flag
 // #define ENABLE_SLAB_RECLAIM 1
 
 /*
  * Internal Slab Header Structure (Definition remains in slab.c)
  * This structure is placed at the beginning of each page allocated for a slab.
  */
 typedef struct slab {
     uint32_t magic;         // Magic number for validation (SLAB_MAGIC)
     struct slab *next;      // Link for slab_partial/slab_full lists in slab_cache_t
     unsigned int free_count;// Number of free objects remaining in this slab
     void *free_list;        // Pointer to the first free object within this slab's data area
     slab_cache_t *cache;    // Back-pointer to the owning cache (optional but useful)
     // Padding might be needed here if struct slab isn't naturally aligned,
     // but usually handled by struct layout. Ensure SLAB_HEADER_ALIGNMENT is sufficient.
 } slab_t;
 
 #define SLAB_HEADER_SIZE ALIGN_UP(sizeof(slab_t), SLAB_HEADER_ALIGNMENT)
 
 
 /* --- struct slab_cache Definition MOVED TO slab.h --- */
 /*
 struct slab_cache {
     const char *name;
     size_t obj_size;            // Size of each object managed by this cache
     unsigned int objs_per_slab;
     slab_t *slab_partial;       // List of partially filled slabs
     slab_t *slab_full;          // List of completely full slabs
     unsigned long alloc_count;
     unsigned long free_count;
     // Add lock/mutex here for SMP safety
 };
 */
 
 /* Forward Declarations */
 static slab_t *slab_create_new(slab_cache_t *cache);
 
 /* Helper to check slab magic */
 static inline bool is_valid_slab(slab_t *slab) {
     if (!slab) {
         terminal_printf("[Slab Check] Error: Encountered NULL slab pointer.\n");
         return false;
     }
     // Check if the slab pointer itself is page-aligned
     if (((uintptr_t)slab & (PAGE_SIZE - 1)) != 0) {
          terminal_printf("[Slab Check] Error: Slab 0x%x is not page aligned!\n", (uintptr_t)slab);
          // This indicates a major issue elsewhere (buddy allocator?)
          return false; // Don't check magic if address is wrong
     }
 
     if (slab->magic != SLAB_MAGIC) {
         terminal_printf("[Slab Check] Error: Invalid slab detected! Addr: 0x%x, Magic: 0x%x (Expected: 0x%x)\n",
                         (uintptr_t)slab, slab->magic, SLAB_MAGIC);
         return false;
     }
     // TODO: Add check for slab->cache back-pointer if implemented?
     return true;
 }
 
 /* slab_create */
 slab_cache_t *slab_create(const char *name, size_t obj_size) {
     if (obj_size == 0) {
         terminal_printf("[Slab] Cache '%s': Error - Object size cannot be 0.\n", name);
         return NULL;
     }
 
     // Ensure object size is large enough to hold a pointer for the free list
     size_t min_size = sizeof(void*);
     if (obj_size < min_size) {
          terminal_printf("[Slab] Cache '%s': Warning - Object size %d too small, increasing to %d.\n",
                          name, (int)obj_size, (int)min_size); // Use %d for size_t if printf limited
          obj_size = min_size;
     }
     // Slab objects themselves should likely be aligned.
     // Ensure obj_size requested is reasonable for alignment.
     obj_size = ALIGN_UP(obj_size, sizeof(void*)); // Align object size up minimally
 
     // Allocate the cache descriptor structure itself using the buddy allocator
     // (since kmalloc isn't ready when caches might be created initially)
     // Or use a dedicated small allocation mechanism if available.
     slab_cache_t *cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
     if (!cache) {
          terminal_printf("[Slab] Cache '%s': Failed buddy_alloc for cache struct (%d bytes).\n", name, sizeof(slab_cache_t));
          return NULL;
     }
 
     // Initialize the cache descriptor
     cache->name             = name; // Should copy name if `name` is not persistent
     cache->obj_size         = obj_size; // Store the final (potentially adjusted/aligned) object size
     cache->objs_per_slab    = 0; // Calculated when first slab is created
     cache->slab_partial     = NULL;
     cache->slab_full        = NULL;
     cache->alloc_count      = 0;
     cache->free_count       = 0;
 
     // terminal_printf("[Slab] Created cache '%s' for object size %d\n", name, (int)cache->obj_size);
     return cache;
 }
 
 /* slab_create_new: Allocates and initializes a new slab (page) for a cache */
 static slab_t *slab_create_new(slab_cache_t *cache) {
     // Allocate one page for the slab metadata + objects
     void *page = buddy_alloc(PAGE_SIZE);
     if (!page) {
         terminal_printf("[Slab] Cache '%s': Failed buddy_alloc for new slab page!\n", cache->name);
         return NULL;
     }
 
     slab_t *slab = (slab_t *)page; // Slab header starts at page beginning
     slab->magic = SLAB_MAGIC;
     slab->next = NULL;
     slab->cache = cache; // Set back-pointer
 
     // Calculate available space after the header
     size_t available_space = PAGE_SIZE - SLAB_HEADER_SIZE;
 
     // Ensure cache object size is valid (should be set in slab_create)
     if (cache->obj_size == 0) {
         terminal_printf("[Slab] ERROR: Cache '%s' obj_size is zero in slab_create_new.\n", cache->name);
         buddy_free(page, PAGE_SIZE);
         return NULL;
     }
 
      // Calculate how many objects fit if not already done (first slab)
     if (cache->objs_per_slab == 0) {
         if (available_space < cache->obj_size) { // Check if even one object fits
              terminal_printf("[Slab] ERROR: Page size too small for header + one object (obj_size %d) in cache '%s'\n",
                             (int)cache->obj_size, cache->name);
              buddy_free(page, PAGE_SIZE);
              return NULL;
         }
         cache->objs_per_slab = (unsigned int)(available_space / cache->obj_size);
     }
 
     // Should not be zero if the check above passed
     if (cache->objs_per_slab == 0) {
          terminal_printf("[Slab] ERROR: Calculated zero objects per slab for cache '%s' (obj_size %d)\n",
                         cache->name, (int)cache->obj_size);
          buddy_free(page, PAGE_SIZE);
          return NULL;
     }
 
     slab->free_count = cache->objs_per_slab; // All objects are initially free
 
     // terminal_printf("  New slab 0x%x for cache '%s': objs=%d\n", (uintptr_t)slab, cache->name, slab->free_count);
 
     // --- Build the free list within the slab ---
     // Objects start after the slab header
     uint8_t *obj_area_start = (uint8_t *)page + SLAB_HEADER_SIZE;
     slab->free_list = (void *)obj_area_start; // Head of list is the first object
 
     for (unsigned int i = 0; i < slab->free_count; i++) {
         void *current_obj = (void *)(obj_area_start + i * cache->obj_size);
         void *next_obj = NULL;
         if (i < (slab->free_count - 1)) { // If not the last object
             next_obj = (void *)(obj_area_start + (i + 1) * cache->obj_size);
         }
         // Write pointer to next free object into the current object's memory space
         *(void **)current_obj = next_obj;
     }
     // terminal_printf("  Built free list starting at 0x%x\n", (uintptr_t)slab->free_list);
 
     return slab;
 }
 
 /* slab_alloc: Allocates one object from the cache */
 void *slab_alloc(slab_cache_t *cache) {
     if (!cache) {
          terminal_write("[Slab] ERROR: slab_alloc called with NULL cache!\n");
          return NULL;
     }
 
     // Add lock here for SMP safety
     // lock_acquire(&cache->lock);
 
     slab_t *slab = cache->slab_partial;
 
     // If no partial slabs, try the full list (shouldn't happen ideally) or create new
     if (!slab) {
         // terminal_printf("[Slab DEBUG] Cache '%s': No partial slabs, creating new.\n", cache->name);
         slab = slab_create_new(cache);
         if (!slab) {
             terminal_printf("[Slab] Cache '%s': Failed to create new slab during allocation.\n", cache->name);
             // lock_release(&cache->lock);
             return NULL; // Allocation failed
         }
         // Add the new slab (which is partially full) to the partial list
         slab->next = NULL; // It's the only one currently
         cache->slab_partial = slab;
         // Fall through to allocate from this new slab
     }
 
     // Validate the chosen slab (should be from partial list or newly created)
     if (!is_valid_slab(slab)) {
         terminal_printf("[Slab] Cache '%s': Corruption detected in slab header 0x%x! Cannot allocate.\n", cache->name, (uintptr_t)slab);
         // TODO: What to do with a corrupt slab? Remove it? Attempt recovery?
         // For now, fail the allocation. Need robust removal from list.
         // lock_release(&cache->lock);
         return NULL;
     }
 
     // Check if the free list is valid before dereferencing
     if (!slab->free_list) {
         // This indicates an internal inconsistency - a slab on the partial list
         // should always have at least one free object and thus a non-NULL free_list.
         terminal_printf("[Slab] Cache '%s': ERROR! Slab 0x%x in partial list has NULL free_list! free_count=%d. State inconsistent.\n",
                         cache->name, (uintptr_t)slab, slab->free_count);
         // Attempt to remove the broken slab from the partial list
         // This assumes 'slab' is definitely cache->slab_partial here. Needs careful list management if not.
         if (cache->slab_partial == slab) {
              cache->slab_partial = slab->next;
         } else {
              // How did we get here if slab wasn't the head? Indicates list traversal needed.
              // Add logic to find and remove 'slab' from cache->slab_partial list if necessary.
              terminal_write("  [Slab] Couldn't easily remove inconsistent slab from partial list head.\n");
         }
         // Don't free the slab's page - it might be corrupted. Maybe leak it for debugging?
         // lock_release(&cache->lock);
         return NULL; // Indicate allocation failure due to inconsistency
     }
 
     // --- Allocate object ---
     void *obj = slab->free_list;               // Get the first free object
     slab->free_list = *(void **)obj;           // Advance free list pointer (read from object memory)
     slab->free_count--;
     cache->alloc_count++;
 
     // terminal_printf("[Slab DEBUG] Cache '%s': Alloc obj 0x%x from slab 0x%x. free_count=%d\n",
     //                cache->name, (uintptr_t)obj, (uintptr_t)slab, slab->free_count);
 
     // --- Update Slab Lists ---
     // If the slab became full after this allocation, move it from partial to full list
     if (slab->free_count == 0) {
         // terminal_printf("  Slab 0x%x is now full, moving to full list.\n", (uintptr_t)slab);
         // Remove from partial list head (most common case)
         if (cache->slab_partial == slab) {
              cache->slab_partial = slab->next;
         } else {
              // Need list traversal to find and remove if not head - requires prev pointer or careful loop
               terminal_write("  [Slab] Warning: Full slab was not head of partial list - removal skipped (needs list traversal).\n");
               // Add proper list removal logic here if this case is possible/expected.
         }
 
         // Add to front of full list
         slab->next = cache->slab_full;
         cache->slab_full = slab;
     }
 
     // Optional: Poison allocated memory before returning pointer to user
     // memset(obj, 0xCC, cache->obj_size);
 
     // lock_release(&cache->lock);
     return obj;
 }
 
 /* slab_free: Frees an object back to its slab cache */
 void slab_free(slab_cache_t *cache, void *obj) {
     if (!cache || !obj) {
          if(!cache) terminal_write("[Slab] Warning: slab_free called with NULL cache.\n");
          if(!obj) terminal_write("[Slab] Warning: slab_free called with NULL object pointer.\n");
          return;
     }
 
     // Add lock here for SMP safety
     // lock_acquire(&cache->lock);
 
     // Calculate the slab base address from the object pointer
     uintptr_t obj_addr = (uintptr_t)obj;
     uintptr_t slab_base = obj_addr & ~(PAGE_SIZE - 1); // Assumes slabs are page-aligned
     slab_t *slab = (slab_t *)slab_base;
 
     // --- Validations ---
     if (!is_valid_slab(slab)) {
         terminal_printf("[Slab] Cache '%s': ERROR freeing obj 0x%x - Invalid slab detected at base 0x%x!\n",
                         cache->name, obj_addr, slab_base);
         // lock_release(&cache->lock);
         return; // Cannot proceed with suspect slab
     }
 
     // Check if slab belongs to the correct cache (if back-pointer exists)
     if (slab->cache != cache) {
          terminal_printf("[Slab] Cache '%s': ERROR freeing obj 0x%x - Slab 0x%x belongs to different cache ('%s')!\n",
                         cache->name, obj_addr, slab_base, slab->cache ? slab->cache->name : "UNKNOWN");
         // lock_release(&cache->lock);
         return; // Freeing to wrong cache!
     }
 
     // Check if object address is within the valid data range for this slab
     uintptr_t data_start = slab_base + SLAB_HEADER_SIZE;
     uintptr_t data_end = data_start + (cache->objs_per_slab * cache->obj_size);
     if (obj_addr < data_start || obj_addr >= data_end) {
          terminal_printf("[Slab] Cache '%s': ERROR freeing obj 0x%x - Address out of range for slab 0x%x [0x%x-0x%x)!\n",
                          cache->name, obj_addr, slab_base, data_start, data_end);
         // lock_release(&cache->lock);
         return;
     }
     // Check if object address is correctly aligned for this cache's object size
     if (((obj_addr - data_start) % cache->obj_size) != 0) {
           terminal_printf("[Slab] Cache '%s': ERROR freeing obj 0x%x - Misaligned within slab 0x%x (obj_size %d)!\n",
                          cache->name, obj_addr, slab_base, (int)cache->obj_size);
         // lock_release(&cache->lock);
         return;
     }
 
     // Optional: Check for double free. Iterate through slab's current free_list. Complex/slow.
     // void *current = slab->free_list;
     // while(current) { if(current == obj) { /* double free! */ return; } current = *(void**)current; }
 
     // Optional: Poison memory being freed
     // memset(obj, 0xDD, cache->obj_size);
 
     // --- Perform Free ---
     // Prepend object to the slab's free list
     *(void **)obj = slab->free_list;
     slab->free_list = obj;
     slab->free_count++;
     cache->free_count++; // Increment cache-wide free count
 
     // terminal_printf("[Slab DEBUG] Cache '%s': Freed obj 0x%x into slab 0x%x (free_count now %d)\n",
     //                cache->name, obj_addr, slab_base, slab->free_count);
 
 
     // --- Update Slab Lists ---
     // If the slab was previously full (free_count was 0, now 1), move it from full list to partial list.
     // If the slab is now completely empty (free_count == objs_per_slab), consider reclaiming it.
     if (slab->free_count == 1) { // It just transitioned from 0 to 1 free
         // terminal_printf("  Slab 0x%x was full, moving to partial list.\n", (uintptr_t)slab);
         // Try to remove from full list
         slab_t **prev = &cache->slab_full;
         slab_t *curr = cache->slab_full;
         bool found_in_full = false;
         while (curr) {
             if (curr == slab) {
                 *prev = curr->next; // Remove from list
                 found_in_full = true;
                 break;
             }
             prev = &curr->next;
             curr = curr->next;
         }
 
         if (found_in_full) {
             // Add to front of partial list
             slab->next = cache->slab_partial;
             cache->slab_partial = slab;
             // terminal_printf("   Moved 0x%x from full -> partial.\n", (uintptr_t)slab);
         } else {
              // This is an error state - free_count became 1, but it wasn't on the full list.
              // It must have already been on the partial list (if free_count was > 0 before)
              // OR the list state is corrupted.
               bool found_in_partial = false;
               curr = cache->slab_partial;
               while(curr) { if(curr == slab) {found_in_partial=true; break;} curr=curr->next; }
 
               if (!found_in_partial) {
                     terminal_printf("[Slab] Cache '%s': ERROR! Slab 0x%x state inconsistent during free (count=1, not in full/partial lists).\n", cache->name, (uintptr_t)slab);
               } else {
                    // Already on partial list, no move needed. This happens if free_count went from e.g. 2 to 3.
                    // terminal_printf("  Slab 0x%x already on partial list (free_count now %d).\n", (uintptr_t)slab, slab->free_count);
               }
         }
     }
     #ifdef ENABLE_SLAB_RECLAIM
     // Optional: Reclaim fully empty slabs
     else if (slab->free_count == cache->objs_per_slab) {
         terminal_printf("  Slab 0x%x is now empty, attempting reclaim...\n", (uintptr_t)slab);
         // Remove from partial list
         slab_t **prev = &cache->slab_partial;
         slab_t *curr = cache->slab_partial;
         bool removed = false;
         while(curr) {
             if (curr == slab) {
                 *prev = curr->next;
                 removed = true;
                 break;
             }
             prev = &curr->next;
             curr = curr->next;
         }
 
         if (removed) {
              terminal_printf("   Removed empty slab 0x%x from partial list, freeing page.\n", (uintptr_t)slab);
              // IMPORTANT: Ensure no pointers to objects within this slab exist anywhere!
              buddy_free((void*)slab_base, PAGE_SIZE); // Free the page back to buddy system
         } else {
              terminal_printf("   ERROR: Could not find empty slab 0x%x in partial list for reclaim!\n", (uintptr_t)slab);
         }
     }
     #endif // ENABLE_SLAB_RECLAIM
 
     // lock_release(&cache->lock);
 }
 
 /* slab_destroy: Frees all slabs associated with a cache and the cache descriptor */
 void slab_destroy(slab_cache_t *cache) {
     if (!cache) return;
 
     terminal_printf("[Slab] Destroying cache '%s'...\n", cache->name);
     // Add lock acquire/release if SMP
 
     // Free all slabs in the partial list
     slab_t *curr = cache->slab_partial;
     slab_t *next;
     while (curr) {
         next = curr->next;
         if (!is_valid_slab(curr)) {
              terminal_printf("  Warning: Corrupt slab 0x%x found in partial list during destroy.\n", (uintptr_t)curr);
         } else {
             terminal_printf("  Freeing partial slab page 0x%x\n", (uintptr_t)curr);
             buddy_free((void *)curr, PAGE_SIZE);
         }
         curr = next;
     }
     cache->slab_partial = NULL;
 
     // Free all slabs in the full list
     curr = cache->slab_full;
     while (curr) {
         next = curr->next;
          if (!is_valid_slab(curr)) {
              terminal_printf("  Warning: Corrupt slab 0x%x found in full list during destroy.\n", (uintptr_t)curr);
         } else {
             terminal_printf("  Freeing full slab page 0x%x\n", (uintptr_t)curr);
             buddy_free((void *)curr, PAGE_SIZE);
         }
         curr = next;
     }
     cache->slab_full = NULL;
 
     // Free the cache descriptor structure itself
     // Use kfree if available and safe, otherwise buddy_free. Buddy is safer early on.
     terminal_printf("  Freeing cache descriptor 0x%x\n", (uintptr_t)cache);
     buddy_free(cache, sizeof(slab_cache_t)); // Assuming allocated with buddy_alloc
 }
 
 /* slab_cache_stats: Retrieves allocation/free counts for a cache */
 void slab_cache_stats(slab_cache_t *cache, unsigned long *out_alloc, unsigned long *out_free) {
     if (!cache) return;
     // Add lock if SMP
     if (out_alloc) *out_alloc = cache->alloc_count;
     if (out_free) *out_free = cache->free_count;
     // Release lock
 }