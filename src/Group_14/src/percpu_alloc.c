/**
 * percpu_alloc.c
 *
 * A world-class, production-style per-CPU allocator for small objects (<= 4KB),
 * leveraging slab caches for each CPU. Larger allocations fall back to buddy.
 *
 * KEY FEATURES:
 *   - Each CPU has an array of slab caches, one per size class (32..4096).
 *   - For a given size <= 4KB, we pick the next size class that fits.
 *   - If the CPU ID is invalid or the size is > 4KB, we fallback to buddy_alloc.
 *   - (Optional) If you want concurrency, each CPU might lock before slab_alloc.
 *   - Additional debugging messages and usage statistics can be enabled.
 */

 #include "percpu_alloc.h"
 #include "slab.h"      // Slab allocator for small objects
 #include "buddy.h"     // Buddy allocator fallback for larger objects
 #include "terminal.h"  // For debug output (remove or #ifdef in production)
 
 #include "types.h">
 
 // ---------------------------------------------------------------------------
 // Constants
 // ---------------------------------------------------------------------------
 
 // The largest size that is handled by slab caching. Larger => buddy fallback.
 #define SMALL_ALLOC_MAX 4096
 
 // The size classes used by the slab allocator. Each CPU has a slab_cache for each class.
 static const size_t kmalloc_size_classes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
 #define NUM_SIZE_CLASSES (sizeof(kmalloc_size_classes) / sizeof(kmalloc_size_classes[0]))
 
 // Max number of CPUs. Adjust for your system if SMP with more than 4.
 #define MAX_CPUS 4
 
 // ---------------------------------------------------------------------------
 // Data Structures
 // ---------------------------------------------------------------------------
 
 /**
  * cpu_allocator
  *
  * Each CPU has an array of slab caches, one for each size class,
  * plus optional usage stats.
  */
 typedef struct cpu_allocator {
     slab_cache_t *kmalloc_caches[NUM_SIZE_CLASSES];
 
     // Optional usage stats: how many allocations/frees happened on that CPU
     uint32_t alloc_count;
     uint32_t free_count;
 } cpu_allocator_t;
 
 // Global array for all CPUs. We index by CPU ID.
 static cpu_allocator_t cpu_allocators[MAX_CPUS];
 
 // ---------------------------------------------------------------------------
 // Internal Helper Functions
 // ---------------------------------------------------------------------------
 
 /**
  * round_up_to_class
  *
  * Rounds up the 'size' to the next supported slab size class.
  * Returns 0 if 'size' > 4096 (i.e. not handled by slab).
  */
 static size_t round_up_to_class(size_t size) {
     for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
         if (size <= kmalloc_size_classes[i]) {
             return kmalloc_size_classes[i];
         }
     }
     return 0;
 }
 
 // ---------------------------------------------------------------------------
 // Public API
 // ---------------------------------------------------------------------------
 
 /**
  * percpu_kmalloc_init
  *
  * Initializes a slab cache for each size class on each CPU.
  * Must be called once at boot, after buddy/slab subsystems are ready.
  *
  * We do not handle concurrency here. In an SMP system, you may want
  * to lock or run this init on the BSP before APs start.
  */
 void percpu_kmalloc_init(void) {
     for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
         // Zero usage stats
         cpu_allocators[cpu].alloc_count = 0;
         cpu_allocators[cpu].free_count  = 0;
 
         for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
             // Create a new slab cache for each size class
             cpu_allocators[cpu].kmalloc_caches[i] =
                 slab_create("percpu_kmalloc_cache", kmalloc_size_classes[i]);
 
             if (!cpu_allocators[cpu].kmalloc_caches[i]) {
                 // For robust error handling, you could fallback or keep track of failure.
                 terminal_write("percpu_kmalloc_init: Failed to create slab cache for CPU ");
                 // Optionally log the CPU number:
                 // (You may add a custom print function to convert int->string.)
             }
         }
     }
     terminal_write("Per-CPU kmalloc initialized.\n");
 }
 
 /**
  * percpu_kmalloc
  *
  * Allocates 'size' bytes on the slab cache for 'cpu_id' if size <= SMALL_ALLOC_MAX,
  * else uses the buddy system. If 'cpu_id' is invalid, fallback to buddy as well.
  *
  * @param size   number of bytes requested
  * @param cpu_id which CPU is calling
  * @return pointer to allocated memory, or NULL on failure
  *
  * NOTE: In an SMP system, you typically want to call this from code that
  * already knows the local CPU ID. You might also need a spinlock if your
  * code can be preempted during allocation.
  */
 void *percpu_kmalloc(size_t size, int cpu_id) {
     if (size == 0) {
         return NULL;
     }
 
     // If cpu_id is out of range, fallback to buddy
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         return buddy_alloc(size);
     }
 
     // If size is within small allocation range => slab
     if (size <= SMALL_ALLOC_MAX) {
         size_t class_size = round_up_to_class(size);
         if (class_size == 0) {
             // Means > 4096, fallback buddy
             return buddy_alloc(size);
         }
         // Find the matching slab cache
         for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
             if (kmalloc_size_classes[i] == class_size) {
                 void *obj = slab_alloc(cpu_allocators[cpu_id].kmalloc_caches[i]);
                 if (!obj) {
                     terminal_write("percpu_kmalloc: slab_alloc failed (CPU ");
                     // optionally print CPU ID
                     terminal_write(")\n");
                     return NULL;
                 }
                 // Update usage stat
                 cpu_allocators[cpu_id].alloc_count++;
                 return obj;
             }
         }
         // Should never get here if round_up_to_class is correct
         return NULL;
     } else {
         // If bigger than 4KB => buddy
         return buddy_alloc(size);
     }
 }
 
 /**
  * percpu_kfree
  *
  * Frees memory previously allocated by percpu_kmalloc (including buddy fallback).
  * We rely on the 'size' to find the correct slab or buddy free.
  *
  * @param ptr    The pointer to free
  * @param size   The size originally passed to percpu_kmalloc
  * @param cpu_id The CPU that did the allocation
  */
 void percpu_kfree(void *ptr, size_t size, int cpu_id) {
     if (!ptr) {
         return; // freeing NULL is no-op
     }
 
     // If CPU is invalid, fallback to buddy
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         buddy_free(ptr, size);
         return;
     }
 
     if (size <= SMALL_ALLOC_MAX) {
         size_t class_size = round_up_to_class(size);
         if (class_size == 0) {
             // Means size > 4096 => buddy
             buddy_free(ptr, size);
             return;
         }
         // Find matching slab
         for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
             if (kmalloc_size_classes[i] == class_size) {
                 slab_free(cpu_allocators[cpu_id].kmalloc_caches[i], ptr);
                 // Update usage stats
                 cpu_allocators[cpu_id].free_count++;
                 return;
             }
         }
         // If no match found, fallback 
         buddy_free(ptr, size);
     } else {
         // Larger => buddy
         buddy_free(ptr, size);
     }
 }
 
 /**
  * percpu_get_stats
  *
  * Optional debugging function to see how many allocations/frees occurred
  * on a particular CPU. You can call this from the kernel shell or logs.
  *
  * @param cpu_id The CPU to query
  * @param out_alloc_count If non-NULL, store the number of successful allocations
  * @param out_free_count  If non-NULL, store the number of frees
  * @return 0 on success, -1 if invalid CPU
  */
 int percpu_get_stats(int cpu_id, uint32_t *out_alloc_count, uint32_t *out_free_count) {
     if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
         return -1;
     }
     if (out_alloc_count) {
         *out_alloc_count = cpu_allocators[cpu_id].alloc_count;
     }
     if (out_free_count) {
         *out_free_count = cpu_allocators[cpu_id].free_count;
     }
     return 0;
 }
 