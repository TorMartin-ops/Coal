/**
 * percpu_alloc.c
 * Per-CPU slab allocator implementation.
 */

 #include "percpu_alloc.h"
 #include "slab.h"
 #include "buddy.h"
 #include "terminal.h"
 #include "types.h"
 #include "kmalloc_internal.h" // Need KALLOC_HEADER_SIZE, KMALLOC_MIN_ALIGNMENT
 #include "paging.h" // For PAGE_SIZE
 
 // ---------------------------------------------------------------------------
 // Constants (Ensure these match kmalloc.c)
 // ---------------------------------------------------------------------------
 #ifndef SLAB_ALLOC_MAX_USER_SIZE // Max *user* size handled by slabs
 #define SLAB_ALLOC_MAX_USER_SIZE 2048
 #endif
 
 // Define size classes based on *total object size* (user + header + align)
 // These caches will store objects of size UP TO the class size.
 // Sizes must accommodate KALLOC_HEADER_SIZE.
 static const size_t percpu_total_size_classes[] = {
     // Calculate based on typical header size (~24 bytes?) + user size
     ALIGN_UP(KALLOC_HEADER_SIZE + 32,   KMALLOC_MIN_ALIGNMENT), // e.g., ~56 -> Cache 64
     ALIGN_UP(KALLOC_HEADER_SIZE + 64,   KMALLOC_MIN_ALIGNMENT), // e.g., ~88 -> Cache 128
     ALIGN_UP(KALLOC_HEADER_SIZE + 128,  KMALLOC_MIN_ALIGNMENT), // e.g., ~152 -> Cache 256? or 192?
     ALIGN_UP(KALLOC_HEADER_SIZE + 256,  KMALLOC_MIN_ALIGNMENT), // e.g., ~280 -> Cache 512?
     ALIGN_UP(KALLOC_HEADER_SIZE + 512,  KMALLOC_MIN_ALIGNMENT),
     ALIGN_UP(KALLOC_HEADER_SIZE + 1024, KMALLOC_MIN_ALIGNMENT),
     ALIGN_UP(KALLOC_HEADER_SIZE + 2048, KMALLOC_MIN_ALIGNMENT) // Max user size 2048
 };
 #define NUM_PERCPU_SIZE_CLASSES (sizeof(percpu_total_size_classes) / sizeof(percpu_total_size_classes[0]))
 
 // Ensure max class size isn't too large for slab allocator
 _Static_assert(percpu_total_size_classes[NUM_PERCPU_SIZE_CLASSES-1] < (PAGE_SIZE - 128), "Largest percpu size class too big for slab");
 
 #ifndef MAX_CPUS
 #define MAX_CPUS 4 // Adjust as needed for your target
    #endif
 
 // ---------------------------------------------------------------------------
 // Data Structures
 // ---------------------------------------------------------------------------
 typedef struct cpu_allocator {
     slab_cache_t *slab_caches[NUM_PERCPU_SIZE_CLASSES];
     uint32_t alloc_count;
     uint32_t free_count;
     char name_buffers[NUM_PERCPU_SIZE_CLASSES][32]; // Buffer to hold generated cache names
 } cpu_allocator_t;
 
 // Array of per-CPU allocator structures
 static cpu_allocator_t cpu_allocators[MAX_CPUS];
 
 // ---------------------------------------------------------------------------
 // Internal Helper - Find matching size class index for *total* size
 // ---------------------------------------------------------------------------
// get_size_class_index_for_total (Use size_t for loop)
static int get_size_class_index_for_total(size_t total_size) {
    // *** Use size_t for loop variable ***
    for (size_t i = 0; i < NUM_PERCPU_SIZE_CLASSES; i++) {
        if (total_size <= percpu_total_size_classes[i]) {
            return (int)i; // Cast index back to int
        }
    }
    return -1;
}
 
 // ---------------------------------------------------------------------------
 // Public API Implementation
 // ---------------------------------------------------------------------------
 
 void percpu_kmalloc_init(void) {
    terminal_write("[percpu] Initializing per-CPU slab caches...\n");
    bool success = true;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_allocators[cpu].alloc_count = 0; cpu_allocators[cpu].free_count = 0;
        // *** Use size_t for loop variable ***
        for (size_t i = 0; i < NUM_PERCPU_SIZE_CLASSES; i++) {
            size_t cache_obj_size = percpu_total_size_classes[i];
            // *** Use snprintf (needs <stdio.h>) ***
            int name_len = snprintf(cpu_allocators[cpu].name_buffers[i], 32, "cpu%d_slab_%u", cpu, (unsigned int)cache_obj_size);
            if (name_len >= 31) cpu_allocators[cpu].name_buffers[i][31] = '\0';
            const char* cache_name = cpu_allocators[cpu].name_buffers[i];

            cpu_allocators[cpu].slab_caches[i] = slab_create(cache_name, cache_obj_size, KMALLOC_MIN_ALIGNMENT, 0, NULL, NULL);
            // ... (Error checking as before) ...
        }
    }
    // ... (Final success/warning message) ...
}
 
 // Modified percpu_kmalloc to accept total size and output cache pointer
 void *percpu_kmalloc(size_t total_required_size, int cpu_id, slab_cache_t **out_cache) {
     // Assumes cpu_id is valid (checked by kmalloc)
     // Assumes total_required_size includes header and is aligned
 
     if (out_cache) *out_cache = NULL; // Default to NULL
 
     // Find the appropriate size class based on the total size needed
     int index = get_size_class_index_for_total(total_required_size);
     if (index < 0) {
         // Total size is too large for any per-cpu slab cache, fallback needed
         return NULL;
     }
 
     slab_cache_t *cache = cpu_allocators[cpu_id].slab_caches[index];
     if (!cache) {
          // terminal_printf("[percpu] Slab cache CPU %d index %d (size %u) not initialized!\n", cpu_id, index, percpu_total_size_classes[index]);
          return NULL; // Cache wasn't created during init
     }
 
     // Check if this cache can actually hold the requested size (sanity check)
      if (cache->internal_slot_size < total_required_size) {
           terminal_printf("[percpu] Internal Error: Cache '%s' slot size %d < required %d !\n",
                           cache->name, cache->internal_slot_size, total_required_size);
           return NULL;
      }
 
     // Attempt to allocate from the specific slab cache for this CPU and size class
     void *obj = slab_alloc(cache); // Returns raw pointer (start of slab object)
     if (obj) {
         cpu_allocators[cpu_id].alloc_count++;
         if (out_cache) *out_cache = cache; // Return the cache pointer used
     } else {
          // Slab allocation failed (e.g., cache full), fallback handled by kmalloc
          // terminal_printf("[percpu] slab_alloc failed for CPU %d cache '%s'. Fallback needed.\n", cpu_id, cache->name);
     }
     return obj;
 }
 
 // Modified percpu_kfree to use cache pointer from header
 void percpu_kfree(void *ptr, slab_cache_t *cache) {
     // Assumes ptr is valid (start of slab object) and cache is correct
     if (!ptr || !cache) {
          terminal_write("[percpu] kfree: Invalid ptr or cache pointer.\n");
          return;
     }
 
     // We need the CPU ID to update stats. Can we derive it from the cache name? Risky.
     // Or does slab_free track stats internally now? Assuming slab_free doesn't track per-cpu stats.
     // For now, we won't update per-cpu free stats here as it's hard to get the CPU ID reliably.
     // TODO: Re-introduce cpu_id tracking if stats are critical.
 
     // Simply call the underlying slab_free
     slab_free(cache, ptr);
 
     // Cannot reliably update cpu_allocators[cpu_id].free_count here without cpu_id
     // Consider adding stats directly to slab_cache_t if needed globally,
     // or store cpu_id in kmalloc_header if per-cpu stats in kfree are required.
 }
 
 // Stats function remains the same
 int percpu_get_stats(int cpu_id, uint32_t *out_alloc_count, uint32_t *out_free_count) {
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         return -1; // Invalid CPU ID
     }
     // These stats might become less accurate if kfree cannot update them easily
     if (out_alloc_count) {
         *out_alloc_count = cpu_allocators[cpu_id].alloc_count;
     }
     if (out_free_count) {
         *out_free_count = cpu_allocators[cpu_id].free_count;
     }
     return 0;
 }