/**
 * slab.c - Slab Allocator Implementation
 * Features: SMP Safety (Spinlocks), Slab Coloring, Footer Canaries, Reclaim Option.
 */

 #include <kernel/memory/slab.h>
 #include <kernel/memory/buddy.h>
 #include <kernel/drivers/display/terminal.h>
 #include <kernel/drivers/display/serial.h>
 #include <kernel/core/types.h>
 #include <kernel/sync/spinlock.h>
 #include <kernel/lib/string.h>
 #include <kernel/memory/paging.h>     // For memset

 #ifndef PAGE_SIZE
 #error "PAGE_SIZE is not defined!"
 #define PAGE_SIZE 4096
 #endif

 // --- Magic Numbers ---
 #define SLAB_HEADER_MAGIC 0xCAFEBABE // Slab metadata magic
 #define SLAB_FOOTER_MAGIC 0xDEADBEEF // Object footer canary

 // Define minimum alignment requirement
 #define SLAB_MIN_ALIGNMENT sizeof(void*)

 // Size of the footer canary
 #define SLAB_FOOTER_SIZE sizeof(uint32_t)

 // Helper macro for alignment calculation
 #define ALIGN_UP(addr, align) (((uintptr_t)(addr) + (align) - 1) & ~((uintptr_t)(align) - 1))

 // --- Feature Flags ---
 #define ENABLE_SLAB_RECLAIM 1 // Return empty slabs to buddy system
 // #define SLAB_POISON_ALLOC 0xCC // Poison allocated objects
 // #define SLAB_POISON_FREE  0xDD // Poison freed objects


 /* Internal Slab Header Structure */
 typedef struct slab {
     uint32_t magic;         // SLAB_HEADER_MAGIC
     struct slab *next;      // Link for lists in slab_cache_t
     unsigned int free_count;// Number of free objects in this slab
     unsigned int objs_this_slab; // Actual number of objs that fit (due to coloring)
     void *free_list;        // Pointer to the first free object
     slab_cache_t *cache;    // Back-pointer to the owning cache
     unsigned int color_offset;// Coloring offset for objects in this slab
     // Padding to ensure header size is multiple of SLAB_MIN_ALIGNMENT
 #define _SLAB_HEADER_CONTENT_SIZE (sizeof(uint32_t) + sizeof(struct slab*) + 2*sizeof(unsigned int) + sizeof(void*) + sizeof(slab_cache_t*) + sizeof(unsigned int))
     char padding[ALIGN_UP(_SLAB_HEADER_CONTENT_SIZE, SLAB_MIN_ALIGNMENT) - _SLAB_HEADER_CONTENT_SIZE];
 #undef _SLAB_HEADER_CONTENT_SIZE
 } slab_t;

 #define SLAB_HEADER_SIZE sizeof(slab_t)
 // _Static_assert((SLAB_HEADER_SIZE % SLAB_MIN_ALIGNMENT) == 0, "slab_t size not aligned");


 /* Forward Declarations */
 static slab_t *slab_grow_cache(slab_cache_t *cache);
 static void slab_list_add(slab_t **list_head, slab_t *slab);
 static bool slab_list_remove(slab_t **list_head, slab_t *slab_to_remove);

 /* Helper: Check slab magic and alignment */
 static inline bool is_valid_slab(const slab_t *slab) {
     if (!slab) { return false; }
     if (((uintptr_t)slab & (PAGE_SIZE - 1)) != 0) { return false; } // Check page alignment
     if (slab->magic != SLAB_HEADER_MAGIC) { return false; } // Check magic
     if (!slab->cache) { return false; } // Check back pointer
     return true;
 }

 /* Helper: Add slab to list head */
 static void slab_list_add(slab_t **list_head, slab_t *slab) {
     slab->next = *list_head;
     *list_head = slab;
 }

 /* Helper: Remove slab from list */
 static bool slab_list_remove(slab_t **list_head, slab_t *slab_to_remove) {
     // ... (implementation as before) ...
     slab_t *current = *list_head;
     slab_t **prev_next_ptr = list_head;
     while (current) {
         if (current == slab_to_remove) {
             *prev_next_ptr = current->next;
             current->next = NULL;
             return true;
         }
         prev_next_ptr = &current->next;
         current = current->next;
     }
     return false;
 }

 /* slab_create */
 slab_cache_t *slab_create(const char *name, size_t obj_size, size_t align,
                           unsigned int color_range,
                           void (*constructor)(void*), void (*destructor)(void*))
 {
     if (obj_size == 0) { /* ... error ... */ return NULL; }
     if (!name) name = "unnamed_slab";

     // --- Determine Alignment ---
     size_t final_align = align ? align : SLAB_MIN_ALIGNMENT;
     if ((final_align & (final_align - 1)) != 0 || final_align == 0) {
         serial_printf("[Slab] Cache '%s': Invalid alignment %d.\n", name, (int)final_align);
         return NULL;
     }

     // --- Calculate Sizes ---
     size_t user_obj_size = obj_size;
     // Ensure space for free list pointer if object is small
     if (user_obj_size < sizeof(void*)) user_obj_size = sizeof(void*);

     // Calculate internal size needed (user size + footer canary)
     size_t internal_size_req = user_obj_size + SLAB_FOOTER_SIZE;
     // Align the internal size
     size_t internal_slot_size = ALIGN_UP(internal_size_req, final_align);

     // --- Allocate Cache Descriptor ---
     slab_cache_t *cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
     if (!cache) { /* ... error ... */ return NULL; }

     // --- Initialize Cache Descriptor ---
     cache->name             = name;
     cache->user_obj_size    = user_obj_size; // Store original request size (or min for ptr)
     cache->internal_slot_size = internal_slot_size;
     cache->alignment        = final_align;
     cache->objs_per_slab_max = 0; // Calculated in grow_cache
     cache->slab_partial     = NULL;
     cache->slab_full        = NULL;
     cache->slab_empty       = NULL;
     cache->color_next       = 0;
     // Ensure color range is valid (e.g., multiple of alignment)
     cache->color_range      = (color_range > 0) ? (color_range & ~(final_align - 1)) : 0;
     if (cache->color_range > PAGE_SIZE / 2) cache->color_range = 0; // Avoid excessive waste
     cache->alloc_count      = 0;
     cache->free_count       = 0;
     cache->constructor      = constructor;
     cache->destructor       = destructor;
     spinlock_init(&cache->lock);

     // --- Final Checks ---
     if (SLAB_HEADER_SIZE + cache->internal_slot_size > PAGE_SIZE) {
         serial_printf("[Slab] Cache '%s': Page size too small for header + one slot (%d + %d > %d).\n",
                        name, (int)SLAB_HEADER_SIZE, (int)cache->internal_slot_size, (int)PAGE_SIZE);
         buddy_free(cache);
         return NULL;
     }

     serial_printf("[Slab] Created cache '%s' (user=%d, slot=%d, align=%d, color=%d)\n",
                     name, (int)cache->user_obj_size, (int)cache->internal_slot_size,
                     (int)cache->alignment, cache->color_range);
     return cache;
 }


 /* slab_grow_cache: Allocates/initializes new slab with coloring */
 static slab_t *slab_grow_cache(slab_cache_t *cache) {
     // --- Lock MUST be held ---

     // *** Release lock BEFORE buddy_alloc ***
     uintptr_t irq_flags = local_irq_save();
     spinlock_release_irqrestore(&cache->lock, irq_flags);

     void *page = buddy_alloc(PAGE_SIZE); // Buddy must be thread-safe

     // *** Re-acquire lock AFTER buddy_alloc ***
     irq_flags = spinlock_acquire_irqsave(&cache->lock);

     if (!page) { /* ... handle error ... */ return NULL; }

     // --- Initialize Slab Header (lock held) ---
     slab_t *slab = (slab_t *)page;
     slab->magic = SLAB_HEADER_MAGIC;
     slab->next = NULL;
     slab->cache = cache;

     // --- Calculate Coloring Offset ---
     if (cache->color_range > 0) {
         // Cycle through offsets, ensuring alignment
         slab->color_offset = (cache->color_next * cache->alignment) % cache->color_range;
         cache->color_next++; // Increment for next slab
     } else {
         slab->color_offset = 0;
     }

     // --- Calculate Objects Fitting in *This* Slab (with coloring) ---
     size_t space_after_header = PAGE_SIZE - SLAB_HEADER_SIZE;
     size_t space_after_color = (space_after_header > slab->color_offset) ? space_after_header - slab->color_offset : 0;
     slab->objs_this_slab = (unsigned int)(space_after_color / cache->internal_slot_size);

     if (slab->objs_this_slab == 0) {
         serial_printf("[Slab] Cache '%s': Error - Zero objects fit slab after coloring (offset %d, slot size %d).\n",
                        cache->name, slab->color_offset, (int)cache->internal_slot_size);
         buddy_free(page); // Free the page
         return NULL; // Indicate failure
     }
     slab->free_count = slab->objs_this_slab;

     // Store max objs per slab if first time
     if (cache->objs_per_slab_max == 0) {
         cache->objs_per_slab_max = slab->objs_this_slab; // Or calc based on 0 offset? Let's use actual first calc.
     }

     // --- Build Free List (with coloring offset) ---
     uint8_t *obj_area_start = (uint8_t *)page + SLAB_HEADER_SIZE + slab->color_offset;
     slab->free_list = (void *)obj_area_start;

     for (unsigned int i = 0; i < slab->free_count; i++) {
         void *current_obj = (void *)(obj_area_start + i * cache->internal_slot_size);
         void *next_obj = (i < slab->free_count - 1) ? (void *)(obj_area_start + (i + 1) * cache->internal_slot_size) : NULL;
         *(void **)current_obj = next_obj;
         // Write initial footer canary for freed objects
         *(uint32_t*)((uintptr_t)current_obj + cache->internal_slot_size - SLAB_FOOTER_SIZE) = SLAB_FOOTER_MAGIC;
         #ifdef SLAB_POISON_FREE
         memset((uint8_t*)current_obj + sizeof(void*), SLAB_POISON_FREE, cache->user_obj_size - sizeof(void*)); // Poison only user area
         #endif
     }

     // Lock is still held, return new slab
     return slab;
 }

 /* slab_alloc */
 void *slab_alloc(slab_cache_t *cache) {
     if (!cache) { /* ... */ return NULL; }

     uintptr_t irq_flags = spinlock_acquire_irqsave(&cache->lock); // *** Acquire Lock ***

     slab_t *slab = cache->slab_partial;
     void *obj = NULL;

     // Try promoting from empty list
     if (!slab && cache->slab_empty) { /* ... promotion logic as before ... */
         slab = cache->slab_empty;
         if (slab_list_remove(&cache->slab_empty, slab)) { slab_list_add(&cache->slab_partial, slab); }
         else { slab = NULL; /* Log error */ }
     }
     // Grow cache if needed
     if (!slab) {
         slab = slab_grow_cache(cache); // Handles lock release/re-acquire for buddy
         if (!slab) { spinlock_release_irqrestore(&cache->lock, irq_flags); return NULL; }
         slab_list_add(&cache->slab_partial, slab);
     }

     // --- Perform Allocation (lock is held) ---
     if (!is_valid_slab(slab) || !slab->free_list) { /* ... handle error ... */ spinlock_release_irqrestore(&cache->lock, irq_flags); return NULL; }

     obj = slab->free_list; // Get raw object slot pointer
     slab->free_list = *(void **)obj; // Advance free list
     slab->free_count--;
     cache->alloc_count++;

     // Update lists if slab became full
     if (slab->free_count == 0) { /* ... move slab partial -> full ... */
          if (slab_list_remove(&cache->slab_partial, slab)) { slab_list_add(&cache->slab_full, slab); }
          else { /* Log error */ }
     }

     // --- Write Footer Canary (inside lock) ---
     *(uint32_t*)((uintptr_t)obj + cache->internal_slot_size - SLAB_FOOTER_SIZE) = SLAB_FOOTER_MAGIC;

     // Call constructor (inside lock)
     if (cache->constructor) {
         cache->constructor(obj); // Pass pointer to start of user area
     }

     spinlock_release_irqrestore(&cache->lock, irq_flags); // *** Release Lock ***

     #ifdef SLAB_POISON_ALLOC
     memset(obj, SLAB_POISON_ALLOC, cache->user_obj_size); // Poison user area only
     #endif

     return obj; // Return pointer to start of user area
 }


 /* slab_free */
 void slab_free(slab_cache_t *provided_cache, void *obj) {
     if (!obj) { return; }

     // --- Initial Validation (before lock) ---
     uintptr_t obj_addr = (uintptr_t)obj; // This is the start of the user area / internal slot
     uintptr_t slab_base = obj_addr & ~(PAGE_SIZE - 1);
     slab_t *slab = (slab_t *)slab_base;
     if (!is_valid_slab(slab)) { /* ... handle error ... */ return; }
     slab_cache_t *cache = slab->cache;
     if (!cache) { /* ... handle error ... */ return; }
     if (provided_cache && provided_cache != cache) { /* ... log warning ... */ }

     // --- Acquire Lock ---
     uintptr_t irq_flags = spinlock_acquire_irqsave(&cache->lock);

     // --- Validations (inside lock) ---
     if (!is_valid_slab(slab) || slab->cache != cache) { /* ... handle error ... */ spinlock_release_irqrestore(&cache->lock, irq_flags); return; }

     // Validate address range and alignment using color offset
     uintptr_t data_start = slab_base + SLAB_HEADER_SIZE + slab->color_offset;
     // Note: objs_this_slab might be smaller than cache->objs_per_slab_max due to coloring
     uintptr_t data_end = data_start + (slab->objs_this_slab * cache->internal_slot_size);
     if (obj_addr < data_start || obj_addr >= data_end || ((obj_addr - data_start) % cache->internal_slot_size) != 0) {
        // <<<<< CORRECTED: Removed erroneous printf, added reasonable one >>>>>
        serial_printf("[Slab] Cache '%s': Invalid free address 0x%lx (Out of bounds or misaligned).\n", cache->name, obj_addr);
        spinlock_release_irqrestore(&cache->lock, irq_flags); return;
     }

     // --- Check Footer Canary (inside lock) ---
     uint32_t *footer_ptr = (uint32_t*)(obj_addr + cache->internal_slot_size - SLAB_FOOTER_SIZE);
     if (*footer_ptr != SLAB_FOOTER_MAGIC) {
          // <<<<< CORRECTED: Changed %x to %lx >>>>>
         serial_printf("[Slab] Cache '%s': CORRUPTION DETECTED freeing obj 0x%lx! Footer magic invalid (Expected: 0x%lx, Found: 0x%lx).\n",
                         cache->name, obj_addr, (unsigned long)SLAB_FOOTER_MAGIC, (unsigned long)*footer_ptr);
         // Optionally: Mark slab as corrupt? Abort? For now, just report and abort free.
         spinlock_release_irqrestore(&cache->lock, irq_flags);
         // Consider a panic or special handling for corrupted memory
         return;
     }

     // Call destructor (inside lock)
     if (cache->destructor) {
         cache->destructor(obj);
     }

     #ifdef SLAB_POISON_FREE
     memset(obj, SLAB_POISON_FREE, cache->user_obj_size); // Poison user area
     // Re-write footer magic after poisoning if needed, or poison around it
     *footer_ptr = SLAB_FOOTER_MAGIC;
     #endif

     // --- Perform Free (inside lock) ---
     *(void **)obj = slab->free_list; // Prepend to free list
     slab->free_list = obj;
     slab->free_count++;
     cache->free_count++;

     // --- Update Slab Lists (inside lock) ---
     bool was_full = (slab->free_count == 1);
     bool is_empty = (slab->free_count == slab->objs_this_slab); // Check against *this slab's* capacity
     bool list_changed = false;

     if (is_empty) { /* ... Move from partial/full to empty/reclaim (as before, use objs_this_slab) ... */
         // Remove from partial or full list
         if (was_full) { list_changed = slab_list_remove(&cache->slab_full, slab); }
         else { list_changed = slab_list_remove(&cache->slab_partial, slab); }

         if (!list_changed && slab->free_count != 1) { // Only error if it wasn't found and wasn't just moved from full
              // <<<<< CORRECTED: Changed %x to %lx >>>>>
              serial_printf("[Slab] Cache '%s': ERROR! Empty slab 0x%lx not found on partial/full list.\n", cache->name, (uintptr_t)slab);
         }

         #ifdef ENABLE_SLAB_RECLAIM
         if (list_changed) {
             spinlock_release_irqrestore(&cache->lock, irq_flags); // Release before buddy_free
             buddy_free((void*)slab_base);
             return; // Slab memory is gone
         }
         #else
         if (list_changed) { slab_list_add(&cache->slab_empty, slab); }
         #endif

     } else if (was_full) { /* ... Move full -> partial (as before) ... */
          if (slab_list_remove(&cache->slab_full, slab)) { slab_list_add(&cache->slab_partial, slab); }
          else { /* Log error */ }
     }

     // --- Release Lock (if not already released by reclaim) ---
     spinlock_release_irqrestore(&cache->lock, irq_flags);
 }


 /* slab_destroy */
 void slab_destroy(slab_cache_t *cache) {
     // ... (Implementation remains largely the same as previous SMP version) ...
     // Needs to acquire lock, detach lists, release lock, free pages, re-acquire lock, free descriptor, restore IRQs.
     if (!cache) return;
     const char * cache_name_copy = cache->name;

     uintptr_t irq_flags = spinlock_acquire_irqsave(&cache->lock);
     serial_printf("[Slab] Destroying cache '%s'...\n", cache_name_copy);
     slab_t *curr, *next;
     int freed_count = 0;
     slab_t **lists[] = {&cache->slab_partial, &cache->slab_full, &cache->slab_empty};
     const char *list_names[] = {"partial", "full", "empty"};

     for (int i = 0; i < 3; ++i) {
         curr = *lists[i]; *lists[i] = NULL; // Detach list head
         spinlock_release_irqrestore(&cache->lock, irq_flags); // Release lock for buddy_free

         while (curr) {
             next = curr->next;
             if (!is_valid_slab(curr)) { /* Log warning */ }
             else { buddy_free((void *)curr); freed_count++; }
             curr = next;
         }
         if (i < 2) { irq_flags = spinlock_acquire_irqsave(&cache->lock); } // Re-acquire for next list/final free
     }
     serial_printf("  Freed %d slab pages.\n", freed_count);

     // Re-acquire lock one last time if necessary before freeing cache struct
      irq_flags = spinlock_acquire_irqsave(&cache->lock);
     buddy_free(cache);
     local_irq_restore(irq_flags); // Restore IRQs after lock struct is gone
     serial_printf("[Slab] Cache '%s' destroyed.\n", cache_name_copy);
 }

 /* slab_cache_stats */
 void slab_cache_stats(slab_cache_t *cache, unsigned long *out_alloc, unsigned long *out_free) {
     // ... (Implementation remains the same - acquire lock, read stats, release lock) ...
     if (!cache) return;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&cache->lock);
     if (out_alloc) *out_alloc = cache->alloc_count;
     if (out_free) *out_free = cache->free_count;
     spinlock_release_irqrestore(&cache->lock, irq_flags);
 }