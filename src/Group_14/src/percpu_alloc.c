#include "percpu_alloc.h"
#include "slab.h"     // Slab allocator interface
#include "buddy.h"    // Buddy allocator interface
#include "terminal.h" // For optional debug output

#include "libc/stddef.h"
#include "libc/stdint.h"
#include "libc/stdbool.h"

// These constants must be in sync with kmalloc.c or your unified allocator design.
#define SMALL_ALLOC_MAX 4096

// Expanded size classes for small allocations.
static const size_t kmalloc_size_classes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
#define NUM_SIZE_CLASSES (sizeof(kmalloc_size_classes) / sizeof(kmalloc_size_classes[0]))

// Maximum number of CPUs supported (adjust as needed).
#define MAX_CPUS 4

// Per-CPU allocator structure, holding a slab cache for each supported size class.
typedef struct cpu_allocator {
    slab_cache_t *kmalloc_caches[NUM_SIZE_CLASSES];
} cpu_allocator_t;

// Global array for per-CPU allocators.
static cpu_allocator_t cpu_allocators[MAX_CPUS];

/**
 * round_up_to_class
 *
 * Rounds up the given size to the next supported allocation class.
 * Returns 0 if the size exceeds the maximum supported for slab allocation.
 */
static size_t round_up_to_class(size_t size) {
    for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (size <= kmalloc_size_classes[i])
            return kmalloc_size_classes[i];
    }
    return 0;
}

/**
 * percpu_kmalloc_init
 *
 * Initializes per–CPU slab caches for small allocations.
 * Must be called during system initialization.
 */
void percpu_kmalloc_init(void) {
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            cpu_allocators[cpu].kmalloc_caches[i] =
                slab_create("percpu_kmalloc_cache", kmalloc_size_classes[i]);
            if (!cpu_allocators[cpu].kmalloc_caches[i]) {
                terminal_write("percpu_kmalloc_init: Failed to create slab cache for CPU ");
                // Optionally, output the CPU number here.
            }
        }
    }
    terminal_write("Per-CPU kmalloc initialized.\n");
}

/**
 * percpu_kmalloc
 *
 * Allocates memory from the per–CPU allocator for the given CPU.
 * For requests <= SMALL_ALLOC_MAX, uses the CPU's slab cache.
 * Larger allocations fall back to the buddy allocator.
 *
 * @param size   The number of bytes requested.
 * @param cpu_id The CPU identifier.
 * @return A pointer to the allocated memory, or NULL on failure.
 */
void *percpu_kmalloc(size_t size, int cpu_id) {
    if (size == 0)
        return NULL;
    if (cpu_id < 0 || cpu_id >= MAX_CPUS)
        return buddy_alloc(size);  // Fallback if invalid CPU ID.
    
    if (size <= SMALL_ALLOC_MAX) {
        size_t class_size = round_up_to_class(size);
        if (class_size == 0)
            return NULL;
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            if (kmalloc_size_classes[i] == class_size) {
                void *obj = slab_alloc(cpu_allocators[cpu_id].kmalloc_caches[i]);
                if (!obj) {
                    terminal_write("percpu_kmalloc: slab_alloc failed for CPU ");
                    // Optionally output the CPU id.
                }
                return obj;
            }
        }
        return NULL; // Should not reach here.
    } else {
        return buddy_alloc(size);
    }
}

/**
 * percpu_kfree
 *
 * Frees memory previously allocated by percpu_kmalloc.
 *
 * @param ptr    Pointer to the allocated memory.
 * @param size   The original allocation size.
 * @param cpu_id The CPU identifier that allocated the memory.
 */
void percpu_kfree(void *ptr, size_t size, int cpu_id) {
    if (!ptr)
        return;
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
        buddy_free(ptr, size);
        return;
    }
    if (size <= SMALL_ALLOC_MAX) {
        size_t class_size = round_up_to_class(size);
        if (class_size == 0)
            return;
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            if (kmalloc_size_classes[i] == class_size) {
                slab_free(cpu_allocators[cpu_id].kmalloc_caches[i], ptr);
                return;
            }
        }
    } else {
        buddy_free(ptr, size);
    }
}
