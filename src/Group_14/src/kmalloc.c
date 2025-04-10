/**
 * kmalloc.c
 *
 * Central kernel memory allocator facade with robust kfree.
 * Uses metadata headers to track allocation type and size.
 */

 #include "kmalloc.h"         // Public API header
 #include "kmalloc_internal.h" // Internal header structure
 #include "buddy.h"            // Underlying page allocator
 #include "terminal.h"         // For logging/debugging
 #include "types.h"
 #include "paging.h"           // For PAGE_SIZE definition
 
 // Include slab or percpu allocator based on configuration
 #ifdef USE_PERCPU_ALLOC
 #   include "percpu_alloc.h"   // Per-CPU slab allocator interface
 #   include "get_cpu_id.h"     // Required to get current CPU ID
 #else
 #   include "slab.h"           // Global slab allocator interface
 #endif
 
 #include <string.h>           // For memset (if poisoning used)
 
 //----------------------------------------------------------------------------
 // Constants and Configuration
 //----------------------------------------------------------------------------
 
 // Max *USER* size that will be attempted via the SLAB allocator.
 #define SLAB_ALLOC_MAX_USER_SIZE 2048
 
 // Define KMALLOC_HEADER_MAGIC for extra validation in kfree (optional)
 // #define KMALLOC_HEADER_MAGIC 0xDEADBEEF
 
 // Add warning if Per-CPU mode is selected but not fully integrated here yet
 #ifdef USE_PERCPU_ALLOC
 #warning "USE_PERCPU_ALLOC is defined, but kmalloc.c requires matching changes in percpu_alloc.c to correctly handle metadata headers for kfree."
 #endif
 
 //----------------------------------------------------------------------------
 // Global Slab Mode Specifics (Only if USE_PERCPU_ALLOC is NOT defined)
 //----------------------------------------------------------------------------
 #ifndef USE_PERCPU_ALLOC
 
 // Define slab classes based on *USER* sizes.
 static const size_t kmalloc_user_size_classes[] = {
     32, 64, 128, 256, 512, 1024, 2048
 };
 #define NUM_KMALLOC_SIZE_CLASSES (sizeof(kmalloc_user_size_classes) / sizeof(kmalloc_user_size_classes[0]))
 
 // Array to hold pointers to the global slab caches.
 static slab_cache_t *global_slab_caches[NUM_KMALLOC_SIZE_CLASSES] = {NULL};
 
 // Statically defined names for the slab caches
 static const char *global_slab_cache_names[NUM_KMALLOC_SIZE_CLASSES] = {
     "g_slab_32", "g_slab_64", "g_slab_128", "g_slab_256",
     "g_slab_512", "g_slab_1024", "g_slab_2048"
 };
 
 
 // Global statistics for slab allocations/frees via kmalloc.
 static uint32_t g_kmalloc_slab_alloc_count = 0;
 static uint32_t g_kmalloc_slab_free_count = 0;
 
 /**
  * @brief Finds the smallest global slab cache that can fit the total required size.
  * @param user_size The requested user size.
  * @param out_total_obj_size Pointer to store the calculated total object size needed.
  * @return Pointer to the slab_cache_t, or NULL if no suitable cache exists (use buddy).
  */
 static slab_cache_t* get_global_slab_cache(size_t user_size, size_t *out_total_obj_size) {
     size_t total_required = ALIGN_UP(KALLOC_HEADER_SIZE + user_size, KMALLOC_MIN_ALIGNMENT);
     if (out_total_obj_size) *out_total_obj_size = total_required;
 
     // Use size_t for loop variable to avoid sign comparison warnings
     for (size_t i = 0; i < NUM_KMALLOC_SIZE_CLASSES; i++) {
         if (user_size <= kmalloc_user_size_classes[i]) {
              if (global_slab_caches[i] && global_slab_caches[i]->obj_size >= total_required) {
                 return global_slab_caches[i];
             }
              // Check only needed if obj_size could somehow be smaller despite matching user_size class
              // else if (global_slab_caches[i]) {
              //    terminal_printf("[kmalloc] WARNING: Cache %s obj_size %d < required %d for user_size %d\n",
              //                   global_slab_cache_names[i], (int)global_slab_caches[i]->obj_size, (int)total_required, (int)user_size);
              //}
         }
     }
     return NULL; // No suitable cache found
 }
 
 #endif // !USE_PERCPU_ALLOC
 
 //----------------------------------------------------------------------------
 // Internal Helper Functions
 //----------------------------------------------------------------------------
 
 /**
  * @brief Calculates the smallest power-of-two size >= total_size for buddy.
  */
 static size_t buddy_get_expected_allocation_size(size_t total_size) {
      if (total_size == 0) return 0;
 
     // Assume PAGE_SIZE is the minimum practical block size from buddy for kmalloc needs
     size_t min_buddy_block = PAGE_SIZE;
     if (total_size < min_buddy_block) total_size = min_buddy_block;
 
     // Check if already power of two
     if ((total_size & (total_size - 1)) == 0) {
         return total_size;
     }
 
     // Calculate next power of two using bit manipulation (efficient)
     // e.g., using leading zero count if available, or a loop
     size_t power_of_2 = min_buddy_block;
     while (power_of_2 < total_size) {
         // Check for potential overflow before shifting
         if (power_of_2 > (SIZE_MAX >> 1)) return (size_t)-1; // Indicate overflow
         power_of_2 <<= 1;
     }
     return power_of_2;
 }
 
 //----------------------------------------------------------------------------
 // Public API Implementation
 //----------------------------------------------------------------------------
 
 void kmalloc_init(void) {
     terminal_write("[kmalloc] Initializing Kmalloc...\n");
     terminal_printf("  - Header Size    : %d bytes\n", (int)KALLOC_HEADER_SIZE); // Cast size_t
     terminal_printf("  - Min Alignment  : %d bytes\n", (int)KMALLOC_MIN_ALIGNMENT); // Cast size_t
     terminal_printf("  - Slab Max User Size: %d bytes\n", (int)SLAB_ALLOC_MAX_USER_SIZE); // Cast size_t
 
 #ifdef USE_PERCPU_ALLOC
     percpu_kmalloc_init();
     terminal_write("[kmalloc] Initialized with Per-CPU strategy.\n");
     // Warning is printed automatically due to #warning directive above
 #else
     terminal_write("[kmalloc] Initializing Global Slab strategy...\n");
     bool overall_success = true;
     g_kmalloc_slab_alloc_count = 0;
     g_kmalloc_slab_free_count = 0;
 
     // Use size_t for loop variable
     for (size_t i = 0; i < NUM_KMALLOC_SIZE_CLASSES; i++) {
         size_t user_class_size = kmalloc_user_size_classes[i];
         size_t cache_obj_size = ALIGN_UP(KALLOC_HEADER_SIZE + user_class_size, KMALLOC_MIN_ALIGNMENT);
         const char *cache_name = global_slab_cache_names[i];
 
         global_slab_caches[i] = slab_create(cache_name, cache_obj_size);
 
         if (!global_slab_caches[i]) {
             terminal_printf("  [ERROR] Failed to create global slab cache '%s' (obj size %d)\n",
                             cache_name, (int)cache_obj_size); // Cast size_t
             overall_success = false;
         } else {
             if (global_slab_caches[i]->obj_size < cache_obj_size) {
                 terminal_printf("  [ERROR] Slab cache '%s' created with obj_size %d < requested %d\n",
                                 cache_name, (int)global_slab_caches[i]->obj_size, (int)cache_obj_size); // Cast size_t
                 slab_destroy(global_slab_caches[i]);
                 global_slab_caches[i] = NULL;
                 overall_success = false;
             } else {
                  terminal_printf("  - Created slab cache '%s' (obj size %d) for user size <= %d\n",
                                 cache_name, (int)global_slab_caches[i]->obj_size, (int)user_class_size); // Cast size_t
             }
         }
     }
     if (overall_success) {
         terminal_write("[kmalloc] Global Slab caches initialized successfully.\n");
     } else {
         terminal_write("[kmalloc] Warning: Some global slab caches failed to initialize.\n");
     }
 #endif // USE_PERCPU_ALLOC
 }
 
 void *kmalloc(size_t user_size) {
     if (user_size == 0) {
         terminal_write("[kmalloc] Warning: Zero size allocation requested.\n");
         return NULL;
     }
 
     void *raw_ptr = NULL;
     kmalloc_header_t *header = NULL;
     size_t allocated_size = 0;
     alloc_type_e alloc_type = ALLOC_TYPE_BUDDY;
     slab_cache_t *slab_cache = NULL;
 
     // --- Strategy Decision ---
     if (user_size <= SLAB_ALLOC_MAX_USER_SIZE) {
 #ifdef USE_PERCPU_ALLOC
         // --- Per-CPU Slab Allocation ---
         // Placeholder - Requires percpu_alloc modifications
         // Calculate total size needed
          size_t total_required = ALIGN_UP(KALLOC_HEADER_SIZE + user_size, KMALLOC_MIN_ALIGNMENT);
          int cpu_id = get_cpu_id(); // Assuming this works
 
          if (cpu_id >= 0) {
              // Ideally: percpu_kmalloc returns {raw_ptr, cache, obj_size}
              // void* percpu_result = percpu_kmalloc(total_required, cpu_id);
              // if (percpu_result) { /* extract info, set header, goto allocation_success */ }
              // else { /* Fallback */ }
               terminal_write("[kmalloc] Per-CPU path: Falling back to buddy (Integration Pending).\n");
               goto use_buddy_allocator; // Temporary fallback
          } else {
               terminal_printf("[kmalloc] Invalid CPU ID %d, using buddy.\n", cpu_id);
               goto use_buddy_allocator;
          }
 
 #else // --- Global Slab Allocation ---
         size_t total_required_size; // Can be ignored here, cache obj_size is definitive
         slab_cache = get_global_slab_cache(user_size, &total_required_size);
         if (slab_cache) {
             raw_ptr = slab_alloc(slab_cache);
             if (raw_ptr) {
                 alloc_type = ALLOC_TYPE_SLAB;
                 allocated_size = slab_cache->obj_size; // Use actual object size from cache
                 g_kmalloc_slab_alloc_count++;
                 goto allocation_success;
             } else {
                 terminal_printf("[kmalloc] Global slab alloc failed (cache '%s'), falling back to buddy.\n", slab_cache->name);
                 // Fall through to buddy
             }
         } // else: No suitable cache found, fall through to buddy
 #endif
     }
 
     // --- Buddy Allocator Path ---
 use_buddy_allocator:
     {
         size_t total_buddy_request_size = KALLOC_HEADER_SIZE + user_size;
         raw_ptr = buddy_alloc(total_buddy_request_size);
         if (!raw_ptr) {
             terminal_printf("[kmalloc] Buddy alloc FAILED for total size %d.\n", (int)total_buddy_request_size); // Cast size_t
             return NULL; // Out of memory
         }
         alloc_type = ALLOC_TYPE_BUDDY;
         slab_cache = NULL; // Not from slab
         allocated_size = buddy_get_expected_allocation_size(total_buddy_request_size);
         if(allocated_size == (size_t)-1 || allocated_size == 0) { // Check overflow/error from helper
             terminal_printf("[kmalloc] Buddy size calculation invalid for req %d.\n", (int)total_buddy_request_size); // Cast size_t
              // Free with the pointer buddy gave, using requested size might be best guess?
              // This situation is tricky, indicates potential inconsistency.
              buddy_free(raw_ptr, total_buddy_request_size); // Attempt free
              return NULL;
         }
     }
 
 // --- Allocation Success: Fill Header ---
 allocation_success:
     if (!raw_ptr) {
          terminal_write("[kmalloc] ERROR: Reached end without valid allocation pointer!\n");
          return NULL;
     }
 
     header = (kmalloc_header_t *)raw_ptr;
     header->allocated_size = allocated_size; // Size expected by buddy_free/slab_free
     header->type = alloc_type;
     header->cache = slab_cache;            // NULL if buddy
 
 #ifdef KMALLOC_HEADER_MAGIC
     header->magic = KMALLOC_HEADER_MAGIC;
 #endif
 
     void *return_ptr = (void *)((uintptr_t)raw_ptr + KALLOC_HEADER_SIZE);
 
     return return_ptr;
 }
 
 
 void kfree(void *ptr) {
     if (ptr == NULL) {
         return;
     }
 
     kmalloc_header_t *header = (kmalloc_header_t *)((uintptr_t)ptr - KALLOC_HEADER_SIZE);
     void* original_ptr = (void*)header;
 
 #ifdef KMALLOC_HEADER_MAGIC
     // Basic check: Is the magic number correct?
     // More advanced checks could involve checking if header pointer is in known heap regions.
     if (header->magic != KMALLOC_HEADER_MAGIC) {
         terminal_printf("[kfree] ERROR: Invalid magic number! ptr=0x%x, header=0x%x\n", (uintptr_t)ptr, (uintptr_t)header);
         // Consider halting or logging corruption. Returning might be dangerous.
         // panic("kfree: invalid magic number detected!");
         return;
     }
 #endif
 
     size_t allocated_size = header->allocated_size;
     alloc_type_e type = header->type;
     slab_cache_t *cache = header->cache;
 
     // Additional sanity checks
     if (type != ALLOC_TYPE_SLAB && type != ALLOC_TYPE_BUDDY) {
          terminal_printf("[kfree] ERROR: Invalid allocation type %d in header! ptr=0x%x\n", type, (uintptr_t)ptr);
          return;
     }
     if (type == ALLOC_TYPE_SLAB && !cache) {
          terminal_printf("[kfree] ERROR: Header type is SLAB but cache pointer is NULL! ptr=0x%x\n", (uintptr_t)ptr);
          return;
     }
      if (type == ALLOC_TYPE_BUDDY && allocated_size == 0) {
           terminal_printf("[kfree] ERROR: Header type is BUDDY but allocated_size is 0! ptr=0x%x\n", (uintptr_t)ptr);
           return;
      }
 
     // Optional: Poison memory before freeing
     // memset(original_ptr, 0xDD, allocated_size);
 
     // --- Call appropriate underlying free function ---
     switch (type) {
         case ALLOC_TYPE_SLAB:
 #ifdef USE_PERCPU_ALLOC
             // Placeholder: Assumes header->cache points to the correct per-CPU cache
             // Requires percpu_kmalloc to store this correctly.
              slab_free(cache, original_ptr);
              // Need a way to update per-CPU free stats if percpu_alloc provides it.
 #else
             slab_free(cache, original_ptr);
             g_kmalloc_slab_free_count++;
 #endif
             break;
 
         case ALLOC_TYPE_BUDDY:
             buddy_free(original_ptr, allocated_size);
             break;
 
         default:
             // Should be unreachable due to checks above
              terminal_printf("[kfree] ERROR: Reached invalid default case! type=%d, ptr=0x%x\n", type, (uintptr_t)ptr);
             break;
     }
 }
 
 void kmalloc_get_global_stats(uint32_t *out_alloc, uint32_t *out_free) {
 #ifndef USE_PERCPU_ALLOC
     if (out_alloc) *out_alloc = g_kmalloc_slab_alloc_count;
     if (out_free) *out_free = g_kmalloc_slab_free_count;
 #else
     if (out_alloc) *out_alloc = 0;
     if (out_free) *out_free = 0;
 #endif
 }