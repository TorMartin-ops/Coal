#ifndef KMALLOC_INTERNAL_H
#define KMALLOC_INTERNAL_H

#include "slab.h" // For slab_cache_t* definition
#include "types.h" // For size_t etc.

// --- Configuration ---

// Define alignment requirement for pointers returned by kmalloc.
// Must be a power of two. Usually pointer size is sufficient.
#ifndef KMALLOC_MIN_ALIGNMENT
#define KMALLOC_MIN_ALIGNMENT sizeof(void*)
#endif

// --- Helper Macro ---
// NOTE: Ensure uintptr_t is defined in types.h or include <stdint.h> if available
#define ALIGN_UP(addr, align) (((uintptr_t)(addr) + (align) - 1) & ~((uintptr_t)(align) - 1))

// --- Metadata Structure ---

// Type of underlying allocator used for a kmalloc allocation
typedef enum {
    ALLOC_TYPE_BUDDY = 1, // Use underlying buddy allocator
    ALLOC_TYPE_SLAB = 2   // Use underlying slab allocator (global or per-cpu)
} alloc_type_e;

// Header placed just before the pointer returned by kmalloc
typedef struct kmalloc_header {
    // Size of the block *as allocated* by the underlying allocator (buddy/slab).
    // For buddy, this is the power-of-two block size.
    // For slab, this is the fixed object size of the slab cache used.
    // This size INCLUDES the kmalloc_header_t itself.
    size_t allocated_size;

    // Indicates which underlying allocator was used.
    alloc_type_e type;

    // Pointer to the specific slab cache this object belongs to.
    // This is NULL if the allocation came directly from the buddy allocator.
    slab_cache_t *cache;

    // Optional: Magic number for validation during kfree. Helps detect corruption
    // or attempts to free non-kmalloc'd pointers. Define KMALLOC_HEADER_MAGIC
    // if you want to enable this (e.g., #define KMALLOC_HEADER_MAGIC 0xDEADBEEF).
#ifdef KMALLOC_HEADER_MAGIC
    uint32_t magic;
#endif

    // --- Padding ---
    // Add explicit padding to ensure sizeof(kmalloc_header_t) is a multiple of KMALLOC_MIN_ALIGNMENT.

    // Calculate the size of fields before padding
#ifdef KMALLOC_HEADER_MAGIC
    #define _KMALLOC_HEADER_CONTENT_SIZE (sizeof(size_t) + sizeof(alloc_type_e) + sizeof(slab_cache_t*) + sizeof(uint32_t))
#else
    #define _KMALLOC_HEADER_CONTENT_SIZE (sizeof(size_t) + sizeof(alloc_type_e) + sizeof(slab_cache_t*))
#endif

    // Calculate padding needed. Size is 0 if already aligned.
    // Relies on standard C allowing zero-size arrays or compiler extensions.
    // If problematic, use #if/#else to define a dummy char if padding is 0.
    char padding[ALIGN_UP(_KMALLOC_HEADER_CONTENT_SIZE, KMALLOC_MIN_ALIGNMENT) - _KMALLOC_HEADER_CONTENT_SIZE];

    #undef _KMALLOC_HEADER_CONTENT_SIZE // Undefine helper macro

} kmalloc_header_t;

// Ensure the final struct size is aligned. Use static assert if available C11+.
// _Static_assert((sizeof(kmalloc_header_t) % KMALLOC_MIN_ALIGNMENT) == 0, "kmalloc_header_t size not aligned");

// Define the constant size of the header for calculations
#define KALLOC_HEADER_SIZE sizeof(kmalloc_header_t)

#endif // KMALLOC_INTERNAL_H