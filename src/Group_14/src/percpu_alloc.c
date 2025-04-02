/**
 * percpu_alloc.c
 *
 * Per-CPU slab allocator implementation. Assumes sizes passed are <= SMALL_ALLOC_MAX.
 * Fallback logic is handled by the higher-level kmalloc.
 */

 #include "percpu_alloc.h"
 #include "slab.h"      // Slab allocator for small objects
 #include "buddy.h"     // Needed only if slab_create fails during init
 #include "terminal.h"  // For debug output
 #include "types.h"     // Common types
 
 // ---------------------------------------------------------------------------
 // Constants (Ensure these match kmalloc.c if defined there)
 // ---------------------------------------------------------------------------
 #ifndef SMALL_ALLOC_MAX
 #define SMALL_ALLOC_MAX 4096
 #endif
 
 // Size classes MUST match those used/checked by kmalloc.c
 static const size_t percpu_size_classes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
 #define NUM_PERCPU_SIZE_CLASSES (sizeof(percpu_size_classes) / sizeof(percpu_size_classes[0]))
 
 #ifndef MAX_CPUS
 #define MAX_CPUS 4 // Adjust as needed
 #endif
 
 // ---------------------------------------------------------------------------
 // Data Structures
 // ---------------------------------------------------------------------------
 typedef struct cpu_allocator {
     slab_cache_t *slab_caches[NUM_PERCPU_SIZE_CLASSES];
     uint32_t alloc_count;
     uint32_t free_count;
 } cpu_allocator_t;
 
 static cpu_allocator_t cpu_allocators[MAX_CPUS];
 
 // ---------------------------------------------------------------------------
 // Internal Helper - Find matching size class index
 // ---------------------------------------------------------------------------
 static int get_size_class_index(size_t size) {
     for (int i = 0; i < NUM_PERCPU_SIZE_CLASSES; i++) {
         if (size <= percpu_size_classes[i]) {
             return i;
         }
     }
     // Should not be reached if size <= SMALL_ALLOC_MAX
     terminal_write("[percpu] Error: Size > SMALL_ALLOC_MAX passed to get_size_class_index.\n");
     return -1;
 }
 
 // ---------------------------------------------------------------------------
 // Public API Implementation
 // ---------------------------------------------------------------------------
 
 void percpu_kmalloc_init(void) {
     terminal_write("[percpu] Initializing per-CPU slab caches...\n");
     bool success = true;
     for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
         cpu_allocators[cpu].alloc_count = 0;
         cpu_allocators[cpu].free_count = 0;
         for (int i = 0; i < NUM_PERCPU_SIZE_CLASSES; i++) {
             // TODO: Generate unique names per CPU/size if desired for debugging
             // e.g., snprintf(cache_name, sizeof(cache_name), "percpu_%d_sz_%d", cpu, percpu_size_classes[i]);
             cpu_allocators[cpu].slab_caches[i] = slab_create("percpu_slab", percpu_size_classes[i]);
             if (!cpu_allocators[cpu].slab_caches[i]) {
                 terminal_printf("[percpu] Failed to create slab cache for CPU %d, size %d\n", cpu, percpu_size_classes[i]);
                 success = false;
                 // Consider cleanup or alternative strategy if init fails
             }
         }
     }
     if (success) {
         terminal_write("[percpu] Per-CPU slab caches initialized.\n");
     } else {
         terminal_write("[percpu] Warning: Some per-CPU slab caches failed to initialize.\n");
     }
 }
 
 void *percpu_kmalloc(size_t size, int cpu_id) {
     // Assumes size > 0, size <= SMALL_ALLOC_MAX, and cpu_id is valid (checked by kmalloc)
     int index = get_size_class_index(size);
     if (index < 0) {
         // This indicates an internal logic error or incorrect size passed.
         return NULL;
     }
 
     slab_cache_t *cache = cpu_allocators[cpu_id].slab_caches[index];
     if (!cache) {
          terminal_printf("[percpu] Slab cache for CPU %d size %d not initialized!\n", cpu_id, percpu_size_classes[index]);
          return NULL; // Cache wasn't created during init
     }
 
     // Attempt to allocate from the specific slab cache for this CPU and size class
     void *obj = slab_alloc(cache);
     if (obj) {
         cpu_allocators[cpu_id].alloc_count++;
     } else {
          // Log slab failure, fallback is handled by kmalloc
          terminal_printf("[percpu] slab_alloc failed for CPU %d size %d. Fallback needed.\n", cpu_id, percpu_size_classes[index]);
     }
     return obj;
 }
 
 void percpu_kfree(void *ptr, size_t size, int cpu_id) {
     // Assumes ptr is valid, size > 0, size <= SMALL_ALLOC_MAX, and cpu_id is valid
     int index = get_size_class_index(size);
     if (index < 0) {
          terminal_write("[percpu] kfree: Invalid size class derived.\n");
         // Cannot determine which slab cache it belonged to. This indicates a major issue.
         // Maybe attempt buddy_free as a last resort? Or panic?
         return;
     }
 
      slab_cache_t *cache = cpu_allocators[cpu_id].slab_caches[index];
      if (!cache) {
           terminal_printf("[percpu] kfree: Slab cache for CPU %d size %d not initialized!\n", cpu_id, percpu_size_classes[index]);
           // Major issue - where did the memory come from?
           return;
      }
 
     slab_free(cache, ptr);
     cpu_allocators[cpu_id].free_count++;
 }
 
 int percpu_get_stats(int cpu_id, uint32_t *out_alloc_count, uint32_t *out_free_count) {
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         return -1; // Invalid CPU ID
     }
     if (out_alloc_count) {
         *out_alloc_count = cpu_allocators[cpu_id].alloc_count;
     }
     if (out_free_count) {
         *out_free_count = cpu_allocators[cpu_id].free_count;
     }
     return 0;
 }