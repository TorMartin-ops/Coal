/**
 * kmalloc.c
 *
 * Central kernel memory allocator. Acts as a facade, choosing between
 * per-CPU slab, global slab, and buddy allocators based on configuration
 * and allocation size. (Revised to handle PAGE_SIZE allocations correctly)
 */

 #include "kmalloc.h"
 #include "buddy.h"      // Underlying page allocator
 #include "terminal.h"   // For logging
 #include "types.h"      // Common types
 #include "paging.h"     // Include for PAGE_SIZE definition
 
 // Include necessary headers based on configuration
 #ifdef USE_PERCPU_ALLOC
 #  include "percpu_alloc.h" // Per-CPU slab allocator interface
 #  include "get_cpu_id.h"   // Required to get current CPU ID
 #else
 #  include "slab.h"         // Global slab allocator interface
 #endif
 
 // ---------------------------------------------------------------------------
 // Constants
 // ---------------------------------------------------------------------------
 
 // Define a practical upper limit for slab allocations.
 // It MUST be less than PAGE_SIZE to allow space for the slab_t header.
 // Reserve at least sizeof(slab_t) + alignment padding. Let's use 64 bytes reserve.
 #define SLAB_ALLOC_MAX_SIZE (PAGE_SIZE - 64)
 
 // ---------------------------------------------------------------------------
 // Global Slab Mode Specifics (Only if USE_PERCPU_ALLOC is NOT defined)
 // ---------------------------------------------------------------------------
 #ifndef USE_PERCPU_ALLOC
 // Size classes for global slabs - MUST NOT include PAGE_SIZE or larger.
 // Adjust the largest size class to be <= SLAB_ALLOC_MAX_SIZE
 static const size_t kmalloc_size_classes[] = {32, 64, 128, 256, 512, 1024, 2048}; // REMOVED 4096
 #define NUM_KMALLOC_SIZE_CLASSES (sizeof(kmalloc_size_classes) / sizeof(kmalloc_size_classes[0]))
 
 static slab_cache_t *global_slab_caches[NUM_KMALLOC_SIZE_CLASSES] = {NULL};
 static uint32_t global_alloc_count = 0;
 static uint32_t global_free_count = 0;
 
 // Helper to find the index for a given size in global mode
 static int get_global_slab_index(size_t size) {
     // Assumes size <= SLAB_ALLOC_MAX_SIZE
     for (int i = 0; i < NUM_KMALLOC_SIZE_CLASSES; i++) {
         if (size <= kmalloc_size_classes[i]) {
             return i;
         }
     }
     // This might happen if size is > last class but <= SLAB_ALLOC_MAX_SIZE
     // In that case, it should fall back to buddy.
     terminal_printf("[kmalloc] Warning: Size %d > largest slab class %d, but <= max %d. Using buddy.\n",
                     size, kmalloc_size_classes[NUM_KMALLOC_SIZE_CLASSES-1], SLAB_ALLOC_MAX_SIZE);
     return -1;
 }
 #endif // !USE_PERCPU_ALLOC
 
 // ---------------------------------------------------------------------------
 // Public API Implementation
 // ---------------------------------------------------------------------------
 
 void kmalloc_init(void) {
 #ifdef USE_PERCPU_ALLOC
     percpu_kmalloc_init(); // Assumes percpu_alloc handles its own size limits appropriately
     terminal_write("[kmalloc] Initialized with Per-CPU strategy.\n");
 #else
     // Initialize global slab caches (only up to max defined class size)
     terminal_write("[kmalloc] Initializing Global Slab strategy...\n");
     bool success = true;
     global_alloc_count = 0; global_free_count = 0;
     for (int i = 0; i < NUM_KMALLOC_SIZE_CLASSES; i++) { // Use correct loop bound
         global_slab_caches[i] = slab_create("global_slab", kmalloc_size_classes[i]);
         if (!global_slab_caches[i]) { /* ... error handling ... */ success = false; }
     }
     if (success) terminal_write("[kmalloc] Initialized Global Slab strategy.\n");
     else terminal_write("[kmalloc] Warning: Some global slab caches failed.\n");
 #endif
 }
 
 void *kmalloc(size_t size) {
     if (size == 0) return NULL;
 
     // ** FIX: Decide strategy based on SLAB_ALLOC_MAX_SIZE **
     if (size <= SLAB_ALLOC_MAX_SIZE) {
         // Attempt Slab allocation (Per-CPU or Global)
 #ifdef USE_PERCPU_ALLOC
         int cpu_id = get_cpu_id();
         if (cpu_id < 0 /* || cpu_id >= MAX_CPUS */) { // MAX_CPUS needs definition if used
              terminal_printf("[kmalloc] Invalid CPU ID %d, fallback buddy.\n", cpu_id);
              return buddy_alloc(size); // Direct buddy alloc if CPU invalid
         }
         void *ptr = percpu_kmalloc(size, cpu_id);
         if (ptr) return ptr; // Per-CPU success
         // Fallback to buddy IF percpu_kmalloc fails
         terminal_printf("[kmalloc] Per-CPU slab failed CPU %d size %d, fallback buddy.\n", cpu_id, size);
         return buddy_alloc(size);
 #else
         // Global Slab Strategy
         int index = get_global_slab_index(size);
         if (index >= 0) { // Fits in a defined global slab class
              slab_cache_t *cache = global_slab_caches[index];
              if (cache) {
                  void *ptr = slab_alloc(cache);
                  if (ptr) { global_alloc_count++; return ptr; } // Global slab success
                  // Fallback to buddy if slab_alloc failed
                  terminal_printf("[kmalloc] Global slab failed size %d, fallback buddy.\n", kmalloc_size_classes[index]);
                  return buddy_alloc(size);
              } else { // Cache wasn't initialized
                   terminal_printf("[kmalloc] Global slab cache %d uninitialized, fallback buddy.\n", index);
                   return buddy_alloc(size);
              }
         } else {
             // Size is <= SLAB_ALLOC_MAX_SIZE but too big for largest defined slab class
             // Fallback to buddy
             terminal_printf("[kmalloc] Size %d fits max slab but not class, fallback buddy.\n", size);
             return buddy_alloc(size);
         }
 #endif
     } else {
         // --- Buddy Allocator Strategy (for large allocations > SLAB_ALLOC_MAX_SIZE) ---
         terminal_printf("[kmalloc] Large allocation (%d bytes > %d), using buddy.\n", size, SLAB_ALLOC_MAX_SIZE);
         return buddy_alloc(size);
     }
 }
 
 void kfree(void *ptr, size_t size) {
     if (!ptr || size == 0) return;
 
     // ** FIX: Decide strategy based on SLAB_ALLOC_MAX_SIZE **
     if (size <= SLAB_ALLOC_MAX_SIZE) {
 #ifdef USE_PERCPU_ALLOC
         // Per-CPU Free (Trusting caller provided correct size and ptr)
         int cpu_id = get_cpu_id();
         // Assuming percpu_kfree handles invalid cpu_id if necessary
         percpu_kfree(ptr, size, cpu_id);
 #else
         // Global Slab Free
         int index = get_global_slab_index(size);
         if (index >= 0) { // Check if it *could* have come from a slab
              slab_cache_t *cache = global_slab_caches[index];
              if (cache) {
                  // Assume it came from slab - relies on slab_free being robust
                  slab_free(cache, ptr);
                  global_free_count++;
                  return; // Done
              }
         }
         // If no slab index or cache not init, assume it came from buddy fallback
         terminal_printf("[kfree] No suitable slab cache for size %d, using buddy_free.\n", size);
         buddy_free(ptr, size);
 #endif
     } else {
         // --- Buddy Free (for large allocations) ---
         buddy_free(ptr, size);
     }
 }
 
 void kmalloc_get_global_stats(uint32_t *out_alloc, uint32_t *out_free) {
     // ... (same as before) ...
     #ifndef USE_PERCPU_ALLOC
     if (out_alloc) *out_alloc = global_alloc_count;
     if (out_free) *out_free = global_free_count;
     #else
     if (out_alloc) *out_alloc = 0; if (out_free) *out_free = 0;
     #endif
 }