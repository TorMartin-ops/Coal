/**
 * percpu_alloc.c
 * Per-CPU slab allocator implementation.
 */

 #include <kernel/memory/percpu_alloc.h>
 #include <kernel/memory/slab.h>
 #include <kernel/memory/buddy.h>
 #include <kernel/drivers/display/terminal.h>
 #include <kernel/drivers/display/serial.h>
 #include <kernel/core/types.h>
 #include <kernel/memory/kmalloc_internal.h> // Need KALLOC_HEADER_SIZE, KMALLOC_MIN_ALIGNMENT, ALIGN_UP
 #include <kernel/memory/paging.h> // For PAGE_SIZE
 #include <libc/stdio.h> // Added for snprintf


 // ---------------------------------------------------------------------------
 // Constants (Ensure these match kmalloc.c)
 // ---------------------------------------------------------------------------
 #ifndef SLAB_ALLOC_MAX_USER_SIZE // Max *user* size handled by slabs
 #define SLAB_ALLOC_MAX_USER_SIZE 2048
 #endif

 // Define size classes based on *total object size* (user + header + align)
 // These caches will store objects of size UP TO the class size.
 // Sizes must accommodate KALLOC_HEADER_SIZE.
 // Ensure this definition is compatible with compile-time constant requirements
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

// This macro definition should follow the array definition at file scope.
#define NUM_PERCPU_SIZE_CLASSES (sizeof(percpu_total_size_classes) / sizeof(percpu_total_size_classes[0]))

 // *** The _Static_assert line that was here has been completely removed. ***

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
    // Use size_t for loop variable
    for (size_t i = 0; i < NUM_PERCPU_SIZE_CLASSES; i++) {
        if (total_size <= percpu_total_size_classes[i]) {
            return (int)i; // Cast index back to int
        }
    }
    return -1; // Size too large for any defined class
}

 // ---------------------------------------------------------------------------
 // Public API Implementation
 // ---------------------------------------------------------------------------

 void percpu_kmalloc_init(void) {
    // Runtime check instead of _Static_assert
    if (percpu_total_size_classes[NUM_PERCPU_SIZE_CLASSES - 1] >= (PAGE_SIZE - 128)) {
        serial_printf("[percpu] FATAL ERROR: Largest percpu size class (%u) too big for slab (PAGE_SIZE %u).\n",
                        (unsigned int)percpu_total_size_classes[NUM_PERCPU_SIZE_CLASSES - 1],
                        (unsigned int)PAGE_SIZE);
        terminal_write("System Halted.\n");
        while(1) { asm volatile("cli; hlt"); } // Halt
    }

    terminal_write("[percpu] Initializing per-CPU slab caches...\n");
    bool success = true;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_allocators[cpu].alloc_count = 0;
        cpu_allocators[cpu].free_count = 0;
        // Use size_t for loop variable
        for (size_t i = 0; i < NUM_PERCPU_SIZE_CLASSES; i++) {
            size_t cache_obj_size = percpu_total_size_classes[i];
            // Use snprintf (ensure it's declared and implemented)
            // TODO: Ensure you have a working snprintf implementation available
            int name_len = snprintf(cpu_allocators[cpu].name_buffers[i], 32, "cpu%d_slab_%u", cpu, (unsigned int)cache_obj_size);
            // Ensure null termination if snprintf truncated
            if (name_len >= 31) cpu_allocators[cpu].name_buffers[i][31] = '\0';
            const char* cache_name = cpu_allocators[cpu].name_buffers[i];

            // Create slab cache: Pass object size, minimum alignment, default color range (0), no constructor/destructor
            cpu_allocators[cpu].slab_caches[i] = slab_create(cache_name, cache_obj_size, KMALLOC_MIN_ALIGNMENT, 0, NULL, NULL);

            if (!cpu_allocators[cpu].slab_caches[i]) {
                 serial_printf("  [ERROR] Failed to create slab cache '%s'\n", cache_name);
                 success = false;
                 // Consider cleaning up successfully created caches for this CPU on failure?
            } else {
                 // Verify the created cache's internal size matches our calculation
                 if (cpu_allocators[cpu].slab_caches[i]->internal_slot_size < cache_obj_size) {
                       serial_printf("  [ERROR] Slab cache '%s' created with internal_slot_size %d < required %d\n",
                                       cache_name, (int)cpu_allocators[cpu].slab_caches[i]->internal_slot_size, (int)cache_obj_size);
                       slab_destroy(cpu_allocators[cpu].slab_caches[i]);
                       cpu_allocators[cpu].slab_caches[i] = NULL;
                       success = false;
                 }
                 // else {
                 //      serial_printf("  - Created slab cache '%s' (slot size %u)\n",
                 //                     cache_name, (unsigned int)cpu_allocators[cpu].slab_caches[i]->internal_slot_size);
                 // }
            }
        }
    }
    if (success) {
         terminal_write("[percpu] Per-CPU slab caches initialized.\n");
    } else {
         terminal_write("[percpu] Warning: Some per-CPU slab caches failed to initialize.\n");
    }
}

 // Modified percpu_kmalloc to accept total size and output cache pointer
 void *percpu_kmalloc(size_t total_required_size, int cpu_id, slab_cache_t **out_cache) {
     // Basic validation
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         serial_printf("[percpu] kmalloc: Invalid CPU ID %d\n", cpu_id);
         if (out_cache) *out_cache = NULL;
         return NULL;
     }
     if (out_cache) *out_cache = NULL; // Default to NULL

     // Find the appropriate size class based on the total size needed
     int index = get_size_class_index_for_total(total_required_size);
     if (index < 0) {
         // Total size is too large for any per-cpu slab cache, fallback needed (handled by kmalloc)
         return NULL;
     }

     slab_cache_t *cache = cpu_allocators[cpu_id].slab_caches[index];
     if (!cache) {
         // Cache wasn't created during init, fallback needed (handled by kmalloc)
         // serial_printf("[percpu] Slab cache CPU %d index %d (size %u) not initialized!\n", cpu_id, index, percpu_total_size_classes[index]);
         return NULL;
     }

     // Sanity check: ensure the selected cache can actually hold the requested size
      if (cache->internal_slot_size < total_required_size) {
           serial_printf("[percpu] Internal Error: Cache '%s' slot size %d < required %d !\n",
                           cache->name, (int)cache->internal_slot_size, (int)total_required_size);
           return NULL; // Should not happen if get_size_class_index_for_total is correct
      }

     // Attempt to allocate from the specific slab cache for this CPU and size class
     void *obj = slab_alloc(cache); // Returns raw pointer (start of slab object)
     if (obj) {
         cpu_allocators[cpu_id].alloc_count++;
         if (out_cache) *out_cache = cache; // Return the cache pointer used
     } else {
         // Slab allocation failed (e.g., cache full), fallback handled by kmalloc
         // serial_printf("[percpu] slab_alloc failed for CPU %d cache '%s'. Fallback needed.\n", cpu_id, cache->name);
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

     // We cannot easily update per-cpu free stats without the CPU ID.
     // TODO: Re-introduce cpu_id tracking in kmalloc_header if needed.

     // Simply call the underlying slab_free
     slab_free(cache, ptr);

     // Update global stats in slab_cache_t if implemented there, or rely on kmalloc's global tracking if not using per-cpu.
 }

 // Stats function remains the same - accuracy depends on kfree implementation
 int percpu_get_stats(int cpu_id, uint32_t *out_alloc_count, uint32_t *out_free_count) {
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         return -1; // Invalid CPU ID
     }
     // These stats might be less accurate if kfree cannot update them easily
     if (out_alloc_count) {
         *out_alloc_count = cpu_allocators[cpu_id].alloc_count;
     }
     if (out_free_count) {
         *out_free_count = cpu_allocators[cpu_id].free_count;
     }
     return 0;
 }