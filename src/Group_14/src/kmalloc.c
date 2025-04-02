/**
 * kmalloc.c
 *
 * A world–class kernel memory allocator supporting two modes:
 *
 *   1) Per-CPU Slab (if USE_PERCPU_ALLOC is defined)
 *      - For small allocations (<= 4KB), we call percpu_kmalloc/cpu_id.
 *      - For larger or invalid CPU, fallback to buddy.
 *
 *   2) Global Slab + Buddy (fallback)
 *      - We create an array of slab caches for class sizes {32,64,128,256,512,1024,2048,4096}.
 *      - If allocation <= 4096, we find the correct slab cache; otherwise buddy fallback.
 *
 * Additional features:
 *   - Thorough documentation
 *   - Optional usage stats (alloc_count, free_count in global mode)
 *   - Enhanced debug logs (buddy fallback messages, creation failures, etc.)
 *   - Concurrency disclaimers (in SMP or interrupts, consider locking or disabling ints)
 */

 #include "kmalloc.h"

 #ifdef USE_PERCPU_ALLOC
 #  include "percpu_alloc.h" // Must define percpu_kmalloc, percpu_kfree
 #  include "buddy.h"        // For fallback
 #  include "terminal.h"
 #  include "types.h"
 
 #else
 #  include "buddy.h"
 #  include "slab.h"
 #  include "terminal.h"
 #  include "types.h"
 #endif
 
 // The maximum size that the slab caches handle (4KB).
 #define SMALL_ALLOC_MAX 4096
 
 // The size classes for slab caching (must be ascending).
 static const size_t kmalloc_size_classes[] = {
     32, 64, 128, 256, 512, 1024, 2048, 4096
 };
 #define NUM_SIZE_CLASSES (sizeof(kmalloc_size_classes)/sizeof(kmalloc_size_classes[0]))
 
 /**
  * round_up_to_class
  *
  * Rounds 'size' up to the next size class <= 4096.
  * If 'size' > 4096, returns 0 => fallback to buddy.
  */
 static size_t round_up_to_class(size_t size) {
     for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
         if (size <= kmalloc_size_classes[i]) {
             return kmalloc_size_classes[i];
         }
     }
     // Means it's bigger than 4096 => buddy fallback
     return 0;
 }
 
 #ifdef USE_PERCPU_ALLOC
 
 //--------------------------------------------------------------------------
 // PER-CPU MODE
 //
 // We rely on percpu_kmalloc_init() to create slab caches for each CPU
 // up to 4KB. Larger => buddy. Must have get_cpu_id() to find local CPU.
 //
 // No usage stats here, unless you add them in percpu_alloc.
 //--------------------------------------------------------------------------
 
 void kmalloc_init(void) {
     percpu_kmalloc_init();
     terminal_write("[kmalloc] Per-CPU unified allocator initialized.\n");
 }
 
 void *kmalloc(size_t size) {
     if (size == 0) return NULL;
 
     // In SMP, you might do "disable interrupts" or lock here for concurrency.
     // This code remains minimal.
 
     int cpu_id = get_cpu_id(); // You must implement get_cpu_id() somewhere
 
     if (size <= SMALL_ALLOC_MAX) {
         return percpu_kmalloc(size, cpu_id);
     } else {
         // buddy fallback
         void *ptr = buddy_alloc(size);
         if (!ptr) {
             terminal_write("[kmalloc] buddy_alloc failed in per-CPU mode.\n");
         }
         return ptr;
     }
 }
 
 void kfree(void *ptr, size_t size) {
     if (!ptr) return;
 
     int cpu_id = get_cpu_id(); 
 
     if (size <= SMALL_ALLOC_MAX) {
         percpu_kfree(ptr, size, cpu_id);
     } else {
         buddy_free(ptr, size);
     }
 }
 
 #else  // GLOBAL SLAB MODE
 
 //--------------------------------------------------------------------------
 // GLOBAL MODE
 //
 // We define a static array of slab caches for size classes up to 4KB.
 // If bigger => buddy. We also track usage stats (alloc_count, free_count).
 //--------------------------------------------------------------------------
 
 // Our global slab caches. Each index is a different size class.
 static slab_cache_t *kmalloc_caches[NUM_SIZE_CLASSES] = { NULL };
 
 // Usage stats
 static uint32_t global_alloc_count = 0;
 static uint32_t global_free_count  = 0;
 
 /**
  * kmalloc_init
  *
  * Creates a slab cache for each class: 32,64,128,256,512,1024,2048,4096.
  * If one fails, we just log and keep going. Then usage stats are reset.
  */
 void kmalloc_init(void) {
     for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
         kmalloc_caches[i] = slab_create("kmalloc_cache", kmalloc_size_classes[i]);
         if (!kmalloc_caches[i]) {
             terminal_write("[kmalloc_init] slab_create failed for size class ");
             // you might convert kmalloc_size_classes[i] to decimal or hex:
             // e.g. print_number(kmalloc_size_classes[i]);
             terminal_write("\n");
         }
     }
     global_alloc_count = 0;
     global_free_count  = 0;
 
     terminal_write("[kmalloc] Global kernel allocator initialized.\n");
 }
 
 /**
  * kmalloc
  *
  * If size <= 4KB => find the matching slab cache. If that fails => buddy fallback.
  * If size > 4KB => buddy directly. We do not track concurrency here, so in a real SMP
  * environment you want a spinlock or interrupt disable around it.
  *
  * @param size The requested size in bytes
  * @return pointer or NULL
  */
 void *kmalloc(size_t size) {
     if (size == 0) return NULL;
 
     if (size <= SMALL_ALLOC_MAX) {
         size_t class_size = round_up_to_class(size);
         if (class_size == 0) {
             // means size > 4096 => buddy
             void *p = buddy_alloc(size);
             if (!p) terminal_write("[kmalloc] buddy_alloc failed!\n");
             return p;
         }
 
         // find the slab cache that matches class_size
         for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
             if (kmalloc_size_classes[i] == class_size) {
                 void *obj = slab_alloc(kmalloc_caches[i]);
                 if (!obj) {
                     terminal_write("[kmalloc] slab_alloc failed, fallback buddy.\n");
                     // fallback
                     void *p = buddy_alloc(size);
                     if (!p) terminal_write("[kmalloc] buddy fallback also failed!\n");
                     return p;
                 }
                 global_alloc_count++;
                 return obj;
             }
         }
         // Should not happen, but fallback if so
         terminal_write("[kmalloc] class_size mismatch? fallback buddy.\n");
         void *p = buddy_alloc(size);
         if (!p) terminal_write("[kmalloc] fallback buddy failed!\n");
         return p;
     } else {
         // buddy for bigger sizes
         void *p = buddy_alloc(size);
         if (!p) terminal_write("[kmalloc] buddy_alloc (large) failed.\n");
         return p;
     }
 }
 
 /**
  * kfree
  *
  * Frees memory previously allocated by kmalloc. We rely on the 'size' 
  * to figure out if it is a slab or buddy block. 
  *
  * @param ptr  pointer from kmalloc
  * @param size same size used in kmalloc
  */
 void kfree(void *ptr, size_t size) {
     if (!ptr) return;
 
     if (size <= SMALL_ALLOC_MAX) {
         size_t class_size = round_up_to_class(size);
         if (class_size == 0) {
             // means size > 4096 => buddy
             buddy_free(ptr, size);
             return;
         }
         // find the correct slab cache
         for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
             if (kmalloc_size_classes[i] == class_size) {
                 slab_free(kmalloc_caches[i], ptr);
                 global_free_count++;
                 return;
             }
         }
         // fallback if not found
         buddy_free(ptr, size);
     } else {
         buddy_free(ptr, size);
     }
 }
 
 /**
  * kmalloc_get_usage
  *
  * Optional function to retrieve how many times we've allocated/freed 
  * in global slab mode.
  *
  * @param out_alloc optional pointer to store global_alloc_count
  * @param out_free  optional pointer to store global_free_count
  */
 void kmalloc_get_usage(uint32_t *out_alloc, uint32_t *out_free) {
     if (out_alloc) {
         *out_alloc = global_alloc_count;
     }
     if (out_free) {
         *out_free = global_free_count;
     }
 }
 
 #endif // USE_PERCPU_ALLOC
 