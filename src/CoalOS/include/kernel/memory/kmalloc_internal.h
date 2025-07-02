#ifndef KMALLOC_INTERNAL_H
#define KMALLOC_INTERNAL_H

#include <kernel/memory/slab.h> // For slab_cache_t* definition
#include <kernel/core/types.h> // For size_t etc.

// Define KMALLOC_HEADER_MAGIC before the struct definition
#define KMALLOC_HEADER_MAGIC 0xDEADBEEF

// --- Configuration ---
#ifndef KMALLOC_MIN_ALIGNMENT
#define KMALLOC_MIN_ALIGNMENT sizeof(void*)
#endif

// --- Helper Macro ---
#define ALIGN_UP(addr, align) (((uintptr_t)(addr) + (align) - 1) & ~((uintptr_t)(align) - 1))

// --- Metadata Structure ---
typedef enum {
    ALLOC_TYPE_BUDDY = 1,
    ALLOC_TYPE_SLAB = 2
} alloc_type_e;

typedef struct kmalloc_header {
    size_t allocated_size; // Actual size allocated by buddy/slab (incl. header)
    alloc_type_e type;
    slab_cache_t *cache;   // Pointer to slab cache (NULL if buddy)
    // Removed cpu_id for now, relying on cache pointer for slab_free

#ifdef KMALLOC_HEADER_MAGIC // Magic member is now included due to the define above
    uint32_t magic;
#endif

    // --- Padding ---
#ifdef KMALLOC_HEADER_MAGIC
    #define _KMALLOC_HEADER_CONTENT_SIZE (sizeof(size_t) + sizeof(alloc_type_e) + sizeof(slab_cache_t*) + sizeof(uint32_t))
#else
    #define _KMALLOC_HEADER_CONTENT_SIZE (sizeof(size_t) + sizeof(alloc_type_e) + sizeof(slab_cache_t*))
#endif
    char padding[ALIGN_UP(_KMALLOC_HEADER_CONTENT_SIZE, KMALLOC_MIN_ALIGNMENT) - _KMALLOC_HEADER_CONTENT_SIZE];
    #undef _KMALLOC_HEADER_CONTENT_SIZE

} kmalloc_header_t;

#define KALLOC_HEADER_SIZE sizeof(kmalloc_header_t)

#endif // KMALLOC_INTERNAL_H