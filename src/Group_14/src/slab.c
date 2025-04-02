/**
 * slab.c - Debug Instrumented Slab Allocator (Revised)
 */

 #include "slab.h"
 #include "buddy.h"
 #include "terminal.h" // Make sure terminal_printf is available
 #include "types.h"
 #include <string.h>  // For memset
 
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096
 #endif
 
 // Define a magic number to help detect slab header corruption
 #define SLAB_MAGIC 0x51AB51AB
 
 // Define alignment requirement (usually pointer size is sufficient)
 #define SLAB_HEADER_ALIGNMENT sizeof(void*)
 
 // Helper macro for alignment calculation
 #define ALIGN_UP(addr, align) (((addr) + (align) - 1) & ~((align) - 1))
 
 
 // #define ENABLE_SLAB_RECLAIM 1
 
 /* Slab Header Structure */
 typedef struct slab {
     uint32_t magic;           // Magic number for validation
     struct slab *next;
     unsigned int free_count;
     void *free_list;          // Pointer to first free object within this slab
     // Add padding if needed by ALIGN_UP, struct layout handles it implicitly here usually
 } slab_t;
 
 /* Slab Cache Structure */
 struct slab_cache {
     const char *name;
     size_t obj_size;            // Size of each object (guaranteed >= sizeof(void*))
     unsigned int objs_per_slab;
     slab_t *slab_partial;       // Slabs with 1 <= free_count < objs_per_slab
     slab_t *slab_full;          // Slabs with free_count == 0
     unsigned long alloc_count;
     unsigned long free_count;
     // Add lock/mutex here for SMP safety
 };
 
 /* Forward Declarations */
 static slab_t *slab_create_new(slab_cache_t *cache);
 
 /* Helper to check slab magic */
 static inline bool is_valid_slab(slab_t *slab) {
     if (!slab) {
         terminal_printf("[Slab Check] Error: Encountered NULL slab pointer.\n");
         return false;
     }
      // Check alignment as well? A slab base must be page aligned.
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
     return true;
 }
 
 /* slab_create */
 slab_cache_t *slab_create(const char *name, size_t obj_size) {
     if (obj_size == 0) {
         terminal_printf("[Slab] Cache '%s': Error - Object size cannot be 0.\n", name);
         return NULL;
     }
 
     // Ensure object size is large enough to hold a pointer for the free list
     // Also, potentially enforce minimum alignment for objects if needed
     size_t min_size = sizeof(void*);
     // size_t min_align = sizeof(void*); // Example alignment requirement
     // if (obj_size < min_align) obj_size = min_align;
     if (obj_size < min_size) {
          terminal_printf("[Slab] Cache '%s': Warning - Object size %d too small, increasing to %d.\n",
                         name, obj_size, min_size);
          obj_size = min_size;
     }
      // Align object size up? Might waste space but can prevent issues.
      // obj_size = ALIGN_UP(obj_size, sizeof(void*));
 
 
     slab_cache_t *cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
     if (!cache) {
          terminal_printf("[Slab] Cache '%s': Failed buddy_alloc for cache struct.\n", name);
          return NULL;
     }
 
     cache->name           = name;
     cache->obj_size       = obj_size; // Use potentially adjusted size
     cache->objs_per_slab  = 0;
     cache->slab_partial   = NULL;
     cache->slab_full      = NULL;
     cache->alloc_count    = 0;
     cache->free_count     = 0;
 
     terminal_printf("[Slab] Created cache '%s' for object size %d\n", name, cache->obj_size);
     return cache;
 }
 
 /* slab_create_new */
 static slab_t *slab_create_new(slab_cache_t *cache) {
     terminal_printf("[Slab DEBUG] Cache '%s': Creating new slab...\n", cache->name);
     void *page = buddy_alloc(PAGE_SIZE);
     if (!page) { /* ... error ... */ terminal_printf("  Failed buddy_alloc!\n"); return NULL; }
 
     slab_t *slab = (slab_t *)page;
     slab->magic = SLAB_MAGIC;
     slab->next = NULL;
 
     // Calculate aligned header size
     size_t header_size = ALIGN_UP(sizeof(slab_t), SLAB_HEADER_ALIGNMENT);
     size_t available   = PAGE_SIZE - header_size;
 
     if (cache->obj_size == 0) { // Safety check
          terminal_printf("[Slab] Error: cache '%s' obj_size is zero in slab_create_new.\n", cache->name);
          buddy_free(page, PAGE_SIZE); return NULL;
     }
 
     // Compute objs_per_slab if not already done (first slab)
     if (cache->objs_per_slab == 0) {
         cache->objs_per_slab = (unsigned int)(available / cache->obj_size);
         if (cache->objs_per_slab == 0) {
              terminal_printf("[Slab] Error: Page size too small for header+object (obj_size %d) in cache '%s'\n", cache->obj_size, cache->name);
              buddy_free(page, PAGE_SIZE); return NULL;
         }
     }
     slab->free_count = cache->objs_per_slab; // Initialize free count
 
     terminal_printf("  New slab 0x%x: header_size=%d, available=%d, obj_size=%d, objs_per_slab=%d\n",
                     (uintptr_t)slab, header_size, available, cache->obj_size, cache->objs_per_slab);
 
     // Build free list starting after the aligned header
     uint8_t *obj_area_start = (uint8_t *)page + header_size;
     slab->free_list = NULL;
 
     if (cache->objs_per_slab > 0) {
         slab->free_list = (void *)obj_area_start; // Head of list is first object
 
         for (unsigned int i = 0; i < cache->objs_per_slab; i++) {
             void *current_obj = (void *)(obj_area_start + i * cache->obj_size);
             void *next_obj = NULL;
             if (i < (cache->objs_per_slab - 1)) { // If not the last object
                 next_obj = (void *)(obj_area_start + (i + 1) * cache->obj_size);
             }
             // Write pointer to next object into the current object's memory space
             *(void **)current_obj = next_obj;
         }
         terminal_printf("  Built free list starting at 0x%x\n", (uintptr_t)slab->free_list);
     }
 
     return slab;
 }
 
 /* slab_alloc */
 void *slab_alloc(slab_cache_t *cache) {
     if (!cache) return NULL;
     // Add lock
 
     slab_t *slab = cache->slab_partial;
 
     // If no partial slabs, try creating one
     if (!slab) {
         terminal_printf("[Slab DEBUG] Cache '%s': No partial slabs, creating new.\n", cache->name);
         slab = slab_create_new(cache);
         if (!slab) { /* ... error handling ... */ return NULL; } // Failed allocation
         // Add new slab to partial list
         slab->next = NULL; // It's the only one
         cache->slab_partial = slab;
     }
 
     // Validate the chosen slab
     if (!is_valid_slab(slab)) {
          terminal_printf("[Slab] Cache '%s': Corruption detected in partial slab header 0x%x! Cannot allocate.\n", cache->name, (uintptr_t)slab);
          // Release lock
          return NULL;
     }
 
     // Check free_list *before* dereferencing it
     if (!slab->free_list) {
         terminal_printf("[Slab] Cache '%s': Error! Slab 0x%x in partial list has NULL free_list! free_count=%d. Removing slab.\n",
                         cache->name, (uintptr_t)slab, slab->free_count);
         // This slab is broken. Remove it from the partial list.
         cache->slab_partial = slab->next;
         // Should we add it to the full list? Or just discard? Discarding is safer.
         // We might leak the slab page if buddy_free isn't called, but better than using corrupt data.
         // Consider adding to a separate "corrupt" list for later inspection?
         // Release lock
         return NULL; // Indicate allocation failure
     }
 
     // Pop object from free list
     void *obj = slab->free_list;
     slab->free_list = *(void **)obj; // Update list head (reads from allocated object)
     slab->free_count--;
     cache->alloc_count++;
 
     terminal_printf("[Slab DEBUG] Cache '%s': Alloc obj 0x%x from slab 0x%x. free_count=%d, new_free_list=0x%x\n",
                     cache->name, (uintptr_t)obj, (uintptr_t)slab, slab->free_count, (uintptr_t)slab->free_list);
 
 
     // If slab is now full, move it from partial to full list
     if (slab->free_count == 0) {
         terminal_printf("  Slab 0x%x is now full, moving to full list.\n", (uintptr_t)slab);
         cache->slab_partial = slab->next; // Remove from partial
         slab->next = cache->slab_full;   // Add to front of full
         cache->slab_full = slab;
     }
 
     // Optional: Poison allocated memory
     // memset(obj, POISON_ALLOCATED, cache->obj_size);
 
     // Release lock
     return obj;
 }
 
 /* slab_free */
 void slab_free(slab_cache_t *cache, void *obj) {
     if (!cache || !obj) return;
     // Add lock
 
     uintptr_t obj_addr = (uintptr_t)obj;
     uintptr_t slab_base = obj_addr & ~(PAGE_SIZE - 1);
     slab_t *slab = (slab_t *)slab_base;
 
     // Validate slab header
     if (!is_valid_slab(slab)) {
         terminal_printf("[Slab] Cache '%s': Error freeing obj 0x%x - Invalid slab magic/alignment at calculated base 0x%x!\n",
                         cache->name, obj_addr, slab_base);
         // Release lock
         return; // Cannot proceed
     }
 
     // Check if object address is within the valid range for this slab
     size_t header_size = ALIGN_UP(sizeof(slab_t), SLAB_HEADER_ALIGNMENT);
     uintptr_t data_start = slab_base + header_size;
     uintptr_t data_end = slab_base + header_size + (cache->objs_per_slab * cache->obj_size);
     if (obj_addr < data_start || obj_addr >= data_end) {
          terminal_printf("[Slab] Cache '%s': Error freeing obj 0x%x - Address out of range for slab 0x%x [0x%x-0x%x)!\n",
                          cache->name, obj_addr, slab_base, data_start, data_end);
          // Release lock
          return;
     }
 
     // Optional: Check for double free by checking if obj is already in free list? Complex.
     // Optional: Check poison pattern if using.
 
     terminal_printf("[Slab DEBUG] Cache '%s': Free obj 0x%x into slab 0x%x (free_count was %d)\n",
                     cache->name, obj_addr, slab_base, slab->free_count);
 
     // Prepend object to the slab's free list
     *(void **)obj = slab->free_list;
     slab->free_list = obj;
     slab->free_count++;
     cache->free_count++;
 
     // Determine if the slab was previously in the full list
     bool was_full = (slab->free_count == 1); // It just went from 0 to 1
 
     if (was_full) {
         terminal_printf("  Slab 0x%x was full, moving to partial list.\n", (uintptr_t)slab);
         // Try to remove from full list
         slab_t **prev = &cache->slab_full;
         slab_t *curr = cache->slab_full;
         bool found_in_full = false;
         while (curr) {
             if (curr == slab) {
                 *prev = curr->next;
                 found_in_full = true;
                 break;
             }
             prev = &curr->next;
             curr = curr->next;
         }
 
         if (found_in_full) {
             // Successfully removed from full, now add to partial
             slab->next = cache->slab_partial;
             cache->slab_partial = slab;
              terminal_printf("   Moved 0x%x from full -> partial.\n", (uintptr_t)slab);
         } else {
              // Slab count went to 1, but it wasn't in the full list.
              // This implies it was already in the partial list (or list corrupted).
              // Check if it's actually in partial list - if so, no action needed.
              bool found_in_partial = false;
              curr = cache->slab_partial;
              while(curr) { if(curr == slab) {found_in_partial=true; break;} curr=curr->next; }
 
              if (!found_in_partial) {
                    // Serious error: Slab's free_count became 1, but it wasn't in full OR partial list.
                   terminal_printf("[Slab] Cache '%s': Error! Slab 0x%x state inconsistent during free (count=1, not in full/partial lists).\n", cache->name, (uintptr_t)slab);
                   // Don't add it anywhere, as state is unknown.
              } else {
                   // Already in partial list, which is expected if free_count was > 0 before this free.
                    terminal_printf("  Slab 0x%x already in partial list (free_count now %d).\n", (uintptr_t)slab, slab->free_count);
              }
         }
     } // End if (was_full)
 
     // Optional: Reclaim fully empty slabs
     #ifdef ENABLE_SLAB_RECLAIM
     // ... (Reclaim logic remains same, ensure list removal is robust) ...
     #endif
 
     // Release lock
 }
 
 /* slab_destroy */
 void slab_destroy(slab_cache_t *cache) { /* ... same as before ... */ }
 
 /* slab_cache_stats */
 void slab_cache_stats(slab_cache_t *cache, unsigned long *out_alloc, unsigned long *out_free) { /* ... same ... */ }