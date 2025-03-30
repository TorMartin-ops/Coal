#include "kmalloc.h"

#ifdef USE_PERCPU_ALLOC
#include "percpu_alloc.h"  // Per-CPU allocator interface
#else
#include "buddy.h"         // Buddy allocator interface for large allocations
#include "slab.h"          // Global slab allocator interface for small allocations
#endif

#include "terminal.h"      // Optional: for debug output

#include "libc/stddef.h"   // Provides size_t, NULL
#include "libc/stdint.h"   // Provides uint32_t, etc.
#include "libc/stdbool.h"  // Provides bool, true, false

/*
 * SMALL_ALLOC_MAX:
 * Allocation requests up to this size (in bytes) are handled by slab caches.
 * Larger allocations fall back to the buddy allocator.
 */
#define SMALL_ALLOC_MAX 4096

/*
 * Supported size classes for slab caches.
 * These sizes must be sorted in increasing order.
 */
static const size_t kmalloc_size_classes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
#define NUM_SIZE_CLASSES (sizeof(kmalloc_size_classes) / sizeof(kmalloc_size_classes[0]))

#ifndef USE_PERCPU_ALLOC
/*
 * Global slab caches used in non-per-CPU mode.
 */
static slab_cache_t *kmalloc_caches[NUM_SIZE_CLASSES] = {0};
#endif

/**
 * round_up_to_class
 *
 * Rounds up the requested size to the next supported allocation class.
 * Returns 0 if the size exceeds the maximum supported for slab allocation.
 *
 * @param size The requested allocation size.
 * @return The size class that should be used.
 */
static size_t round_up_to_class(size_t size) {
    for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (size <= kmalloc_size_classes[i])
            return kmalloc_size_classes[i];
    }
    return 0;
}

#ifdef USE_PERCPU_ALLOC
/*
 * In per-CPU mode, the per-CPU allocator (provided by percpu_alloc.h)
 * handles small allocations. The following functions assume that a function
 * get_cpu_id() exists that returns the current CPU's ID (0..MAX_CPUS-1).
 */

void kmalloc_init(void) {
    percpu_kmalloc_init();
    terminal_write("kmalloc: Per-CPU unified allocator initialized.\n");
}

void *kmalloc(size_t size) {
    int cpu_id = get_cpu_id(); // Must be implemented elsewhere.
    
    if (size == 0)
        return NULL;
    
    if (size <= SMALL_ALLOC_MAX) {
        return percpu_kmalloc(size, cpu_id);
    } else {
        return buddy_alloc(size);
    }
}

void kfree(void *ptr, size_t size) {
    int cpu_id = get_cpu_id(); // Must be implemented elsewhere.
    
    if (!ptr)
        return;
    
    if (size <= SMALL_ALLOC_MAX) {
        percpu_kfree(ptr, size, cpu_id);
    } else {
        buddy_free(ptr, size);
    }
}
#else
/*
 * Global allocator mode (fallback when per-CPU allocation is not enabled).
 */
void kmalloc_init(void) {
    for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
        kmalloc_caches[i] = slab_create("kmalloc_cache", kmalloc_size_classes[i]);
        if (!kmalloc_caches[i]) {
            terminal_write("kmalloc_init: Failed to create slab cache for size ");
            // Optionally, output kmalloc_size_classes[i] here.
        }
    }
    terminal_write("kmalloc: Unified kernel allocator initialized.\n");
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    
    if (size <= SMALL_ALLOC_MAX) {
        size_t class_size = round_up_to_class(size);
        if (class_size == 0)
            return NULL;
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            if (kmalloc_size_classes[i] == class_size) {
                void *obj = slab_alloc(kmalloc_caches[i]);
                if (!obj) {
                    terminal_write("kmalloc: slab_alloc failed for class ");
                    // Optionally output class_size.
                }
                return obj;
            }
        }
        return NULL; // Should not reach here.
    } else {
        return buddy_alloc(size);
    }
}

void kfree(void *ptr, size_t size) {
    if (!ptr)
        return;
    
    if (size <= SMALL_ALLOC_MAX) {
        size_t class_size = round_up_to_class(size);
        if (class_size == 0)
            return;
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            if (kmalloc_size_classes[i] == class_size) {
                slab_free(kmalloc_caches[i], ptr);
                return;
            }
        }
    } else {
        buddy_free(ptr, size);
    }
}
#endif
