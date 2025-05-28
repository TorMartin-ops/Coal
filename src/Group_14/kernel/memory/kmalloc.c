/**
 * kmalloc.c
 * Central kernel memory allocator facade.
 * Uses per-CPU slab caches if enabled, otherwise global slab or buddy.
 */

 #include <kernel/memory/kmalloc.h>
 #include <kernel/memory/kmalloc_internal.h>
 #include <kernel/memory/buddy.h>
 #include <kernel/drivers/display/terminal.h>
 #include <kernel/core/types.h>
 #include <kernel/memory/paging.h> // For PAGE_SIZE definition
 #include <libc/stdint.h> // Corrected include path

 
 // *** Enable Per-CPU strategy ***
 #define USE_PERCPU_ALLOC
 
 #ifdef USE_PERCPU_ALLOC
 #   include <kernel/memory/percpu_alloc.h>
 #   include <kernel/cpu/get_cpu_id.h>
 #else // Fallback to global slab
 #   include <kernel/memory/slab.h>
 #endif
 
 #include <kernel/lib/string.h> // For memset
 
 //----------------------------------------------------------------------------
 // Constants and Configuration
 //----------------------------------------------------------------------------
 
 // Max *USER* size eligible for SLAB allocation (per-cpu or global).
 // Should be less than PAGE_SIZE to accommodate slab_t header if buddy fallback occurs.
 // Let's keep it at 2048 for now.
 #define SLAB_ALLOC_MAX_USER_SIZE 2048
 _Static_assert(SLAB_ALLOC_MAX_USER_SIZE < (PAGE_SIZE - 128), "SLAB_ALLOC_MAX_USER_SIZE too large"); // Basic check
 
 // Define KMALLOC_HEADER_MAGIC for extra validation in kfree (optional)
 #define KMALLOC_HEADER_MAGIC 0xDEADBEEF
 
 //----------------------------------------------------------------------------
 // Global Slab Mode Specifics (Only if USE_PERCPU_ALLOC is NOT defined)
 //----------------------------------------------------------------------------
 #ifndef USE_PERCPU_ALLOC
 
 // Define slab classes based on *USER* sizes.
 static const size_t kmalloc_user_size_classes[] = {
     32, 64, 128, 256, 512, 1024, 2048 // Up to SLAB_ALLOC_MAX_USER_SIZE
 };
 #define NUM_KMALLOC_SIZE_CLASSES (sizeof(kmalloc_user_size_classes) / sizeof(kmalloc_user_size_classes[0]))
 
 static slab_cache_t *global_slab_caches[NUM_KMALLOC_SIZE_CLASSES] = {NULL};
 static const char *global_slab_cache_names[NUM_KMALLOC_SIZE_CLASSES] = {
     "g_slab_32", "g_slab_64", "g_slab_128", "g_slab_256",
     "g_slab_512", "g_slab_1024", "g_slab_2048"
 };
 static uint32_t g_kmalloc_slab_alloc_count = 0;
 static uint32_t g_kmalloc_slab_free_count = 0;
 
 /**
  * @brief Finds smallest global slab cache for the *total* size needed.
  * @param total_required_size Size needed (user + header, aligned).
  * @return Pointer to the slab_cache_t, or NULL.
  */
 static slab_cache_t* get_global_slab_cache(size_t total_required_size) {
     for (size_t i = 0; i < NUM_KMALLOC_SIZE_CLASSES; i++) {
          // Check if the cache exists and its object size is sufficient
          if (global_slab_caches[i] && global_slab_caches[i]->internal_slot_size >= total_required_size) {
             // Check if this cache is the *best* fit (i.e., previous wasn't smaller and sufficient)
             // This assumes kmalloc_user_size_classes maps directly to cache suitability.
             // A more robust check might compare internal_slot_size directly.
             if (i == 0 || !global_slab_caches[i-1] || global_slab_caches[i-1]->internal_slot_size < total_required_size) {
                 return global_slab_caches[i];
             }
          }
     }
     return NULL; // No suitable cache found
 }
 #endif // !USE_PERCPU_ALLOC
 
 //----------------------------------------------------------------------------
 // Internal Helper Functions
 //----------------------------------------------------------------------------
 
 // Calculates the expected power-of-two size buddy_alloc will return
 static size_t buddy_get_expected_allocation_size(size_t total_size) {
    if (total_size == 0) return 0;
    // *** Use MIN_BLOCK_SIZE from buddy.h ***
    size_t min_practical_block = (MIN_BLOCK_SIZE > KMALLOC_MIN_ALIGNMENT) ? MIN_BLOCK_SIZE : KMALLOC_MIN_ALIGNMENT;
    if (total_size < min_practical_block) total_size = min_practical_block;

    size_t power_of_2 = min_practical_block;
    while (power_of_2 < total_size) {
        // *** Use SIZE_MAX ***
        if (power_of_2 > (SIZE_MAX >> 1)) return SIZE_MAX;
        power_of_2 <<= 1;
    }
    // *** Use buddy_size_to_order from buddy.h ***
    // Note: Ensure buddy_size_to_order is declared in buddy.h if used here
    // If buddy_size_to_order is static in buddy.c, this helper needs adjustment
    // or buddy_size_to_order needs to be made non-static or accessible.
    // For now, assuming it's accessible or logic is duplicated/simplified here.
    // Simple check based on MAX_ORDER instead of calling buddy_size_to_order:
    if (power_of_2 > ((size_t)1 << MAX_ORDER)) {
         return SIZE_MAX;
    }
    return power_of_2;
}
 
 //----------------------------------------------------------------------------
 // Public API Implementation
 //----------------------------------------------------------------------------
 
 void kmalloc_init(void) {
     terminal_write("[kmalloc] Initializing Kmalloc...\n");
     serial_printf("  - Header Size    : %d bytes\n", (int)KALLOC_HEADER_SIZE);
     serial_printf("  - Min Alignment  : %d bytes\n", (int)KMALLOC_MIN_ALIGNMENT);
     serial_printf("  - Slab Max User Size: %d bytes\n", (int)SLAB_ALLOC_MAX_USER_SIZE);
 
 #ifdef USE_PERCPU_ALLOC
     terminal_write("[kmalloc] Initializing Per-CPU strategy...\n");
     percpu_kmalloc_init(); // Calls slab_create internally for each CPU/size
     terminal_write("[kmalloc] Per-CPU caches initialized.\n");
 #else
     terminal_write("[kmalloc] Initializing Global Slab strategy...\n");
     bool overall_success = true;
     g_kmalloc_slab_alloc_count = 0;
     g_kmalloc_slab_free_count = 0;
 
     for (size_t i = 0; i < NUM_KMALLOC_SIZE_CLASSES; i++) {
         size_t user_class_size = kmalloc_user_size_classes[i];
         // Create slab caches to hold the object PLUS our header
         size_t cache_obj_size = ALIGN_UP(KALLOC_HEADER_SIZE + user_class_size, KMALLOC_MIN_ALIGNMENT);
         const char *cache_name = global_slab_cache_names[i];
 
         // Pass object size, minimum alignment, default color range (0), no constructor/destructor
         global_slab_caches[i] = slab_create(cache_name, cache_obj_size, KMALLOC_MIN_ALIGNMENT, 0, NULL, NULL);
 
         if (!global_slab_caches[i]) {
             serial_printf("  [ERROR] Failed to create global slab cache '%s' (obj size %d)\n",
                             cache_name, (int)cache_obj_size);
             overall_success = false;
         } else {
             // Verify the created cache's internal size matches our calculation
             if (global_slab_caches[i]->internal_slot_size < cache_obj_size) {
                  serial_printf("  [ERROR] Slab cache '%s' created with internal_slot_size %d < requested %d\n",
                                  cache_name, (int)global_slab_caches[i]->internal_slot_size, (int)cache_obj_size);
                  slab_destroy(global_slab_caches[i]);
                  global_slab_caches[i] = NULL;
                  overall_success = false;
              } else {
                   serial_printf("  - Created slab cache '%s' (slot size %d) for user size <= %d\n",
                                  cache_name, (int)global_slab_caches[i]->internal_slot_size, (int)user_class_size);
              }
         }
     }
     if (!overall_success) {
         terminal_write("[kmalloc] Warning: Some global slab caches failed to initialize.\n");
     } else {
          terminal_write("[kmalloc] Global Slab caches initialized successfully.\n");
     }
 #endif // USE_PERCPU_ALLOC
 }
 
 void *kmalloc(size_t user_size) {
    if (user_size == 0) return NULL;
    size_t total_required_size = ALIGN_UP(KALLOC_HEADER_SIZE + user_size, KMALLOC_MIN_ALIGNMENT);

    void *raw_ptr = NULL;
    kmalloc_header_t *header = NULL;
    size_t actual_alloc_size = 0;
    alloc_type_e alloc_type = ALLOC_TYPE_BUDDY;
    slab_cache_t *slab_cache = NULL;

    if (user_size <= SLAB_ALLOC_MAX_USER_SIZE) {
#ifdef USE_PERCPU_ALLOC
        int cpu_id = get_cpu_id();
        if (cpu_id >= 0) {
            raw_ptr = percpu_kmalloc(total_required_size, cpu_id, &slab_cache);
            if (raw_ptr && slab_cache) {
                alloc_type = ALLOC_TYPE_SLAB;
                actual_alloc_size = slab_cache->internal_slot_size;
                goto allocation_success;
            } // else fallback...
        } // else fallback...
#else
        // Global Slab logic
        slab_cache_t* global_cache = get_global_slab_cache(total_required_size);
        if (global_cache) {
             raw_ptr = slab_alloc(global_cache);
             if (raw_ptr) {
                  alloc_type = ALLOC_TYPE_SLAB;
                  slab_cache = global_cache;
                  actual_alloc_size = global_cache->internal_slot_size;
                  g_kmalloc_slab_alloc_count++;
                  goto allocation_success;
             } // else fallback...
        } // else fallback...
#endif
    }

    // Buddy Allocator Path
    actual_alloc_size = buddy_get_expected_allocation_size(total_required_size);
    if (actual_alloc_size == SIZE_MAX) { return NULL; } // Use SIZE_MAX check
    raw_ptr = BUDDY_ALLOC(actual_alloc_size); // Use the macro if DEBUG_BUDDY is defined
    if (!raw_ptr) { return NULL; }
    alloc_type = ALLOC_TYPE_BUDDY;
    slab_cache = NULL; // Ensure slab_cache is NULL for buddy allocs

allocation_success:
    if (!raw_ptr) { return NULL; }
    header = (kmalloc_header_t *)raw_ptr;
    header->allocated_size = actual_alloc_size;
    header->type = alloc_type;
    header->cache = slab_cache; // Store the slab cache pointer (or NULL if buddy)

    #ifdef KMALLOC_HEADER_MAGIC
    header->magic = KMALLOC_HEADER_MAGIC;
    #endif

    // Zero the user area (optional, but good practice)
    // memset((void *)((uintptr_t)raw_ptr + KALLOC_HEADER_SIZE), 0, user_size);

    return (void *)((uintptr_t)raw_ptr + KALLOC_HEADER_SIZE); // Return pointer to user area
}
 
 
void kfree(void *ptr) {
    if (ptr == NULL) return;
    kmalloc_header_t *header = (kmalloc_header_t *)((uintptr_t)ptr - KALLOC_HEADER_SIZE);
    void* original_alloc_ptr = (void*)header;

    #ifdef KMALLOC_HEADER_MAGIC
    if (header->magic != KMALLOC_HEADER_MAGIC) {
        serial_printf("[kfree] Error: Invalid magic number on free at %p (header %p)!\n", ptr, header);
        // Optionally: Panic or log more details
        return; // Don't proceed with free
    }
    #endif

    size_t allocated_size = header->allocated_size;
    alloc_type_e type = header->type;
    slab_cache_t *cache = header->cache;

    // Consistency checks
    if (type != ALLOC_TYPE_SLAB && type != ALLOC_TYPE_BUDDY) {
        serial_printf("[kfree] Error: Invalid alloc type %d in header at 0x%x.\n", type, (uintptr_t)header);
        return;
    }
    if (type == ALLOC_TYPE_SLAB && !cache) {
         serial_printf("[kfree] Error: Slab alloc type but NULL cache pointer in header at 0x%x.\n", (uintptr_t)header);
        return;
    }
    if (type == ALLOC_TYPE_BUDDY && allocated_size == 0) {
         serial_printf("[kfree] Error: Buddy alloc type but zero size in header at 0x%x.\n", (uintptr_t)header);
        return;
    }
    // Optional: Add check if slab cache's slot size matches header->allocated_size?


    // Clear magic before freeing (helps detect double frees if magic is checked)
    #ifdef KMALLOC_HEADER_MAGIC
    header->magic = 0; // Invalidate magic
    #endif

    switch (type) {
        case ALLOC_TYPE_SLAB:
#ifdef USE_PERCPU_ALLOC
            percpu_kfree(original_alloc_ptr, cache);
#else
            slab_free(cache, original_alloc_ptr);
            g_kmalloc_slab_free_count++;
#endif
            break;
        case ALLOC_TYPE_BUDDY:
             #ifdef DEBUG_BUDDY
             // Debug buddy free macro doesn't need size
             BUDDY_FREE(original_alloc_ptr);
             #else
             // Non-debug buddy free needs the allocated size
             buddy_free(original_alloc_ptr);
             #endif
            break;
        default:
             serial_printf("[kfree] Error: Reached default case in switch (type %d).\n", type);
             break; // Should not happen
    }
}
 
 // Function remains the same, only relevant for global slab mode
 void kmalloc_get_global_stats(uint32_t *out_alloc, uint32_t *out_free) {
 #ifndef USE_PERCPU_ALLOC
     if (out_alloc) *out_alloc = g_kmalloc_slab_alloc_count;
     if (out_free) *out_free = g_kmalloc_slab_free_count;
 #else
     // In per-CPU mode, these global stats are not maintained here.
     // Use percpu_get_stats for per-CPU details.
     if (out_alloc) *out_alloc = 0;
     if (out_free) *out_free = 0;
 #endif
 }