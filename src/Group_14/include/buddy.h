#ifndef BUDDY_H
#define BUDDY_H

#include "types.h" // Includes size_t, bool, uintptr_t, etc.

// Define configuration constants here
#define MIN_ORDER 5  // Smallest block order (2^5 = 32 bytes) - Ensure (1 << MIN_ORDER) >= DEFAULT_ALIGNMENT
#define MAX_ORDER 22 // Largest block order (2^22 = 4MB) - ADJUST AS NEEDED

// Define default alignment requirement for the architecture
#define DEFAULT_ALIGNMENT 8 // e.g., 8 bytes for 32/64-bit systems
#define MIN_BLOCK_SIZE (1 << MIN_ORDER)

// --- Optional Debug Feature ---
// Define DEBUG_BUDDY (e.g., via build flags -DDEBUG_BUDDY) to enable tracking
// #define DEBUG_BUDDY 1

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the buddy memory allocator.
 *
 * Sets up the free lists for the buddy system within the provided memory region.
 * The region must be large enough to hold at least one block of the smallest order.
 *
 * @param heap Pointer to the start of the memory region to manage.
 * @param size The total size of the memory region in bytes.
 */
void buddy_init(void *heap, size_t size);

#ifdef DEBUG_BUDDY
/**
 * @brief Allocates a block of memory of at least 'size' bytes (Debug Version).
 * Records allocation details for leak detection. Use the BUDDY_ALLOC macro.
 *
 * @param size The minimum number of bytes required.
 * @param file The source file where the allocation is requested.
 * @param line The line number where the allocation is requested.
 * @return Pointer to the allocated memory block (aligned), or NULL if allocation fails.
 */
void *buddy_alloc_internal(size_t size, const char* file, int line);

/**
 * @brief Frees a block of memory previously allocated by buddy_alloc (Debug Version).
 * Verifies the pointer against the tracking list before freeing. Use the BUDDY_FREE macro.
 *
 * @param ptr Pointer to the memory block to free.
 * @param file The source file where the free is requested.
 * @param line The line number where the free is requested.
 */
void buddy_free_internal(void *ptr, const char* file, int line);

/**
 * @brief Dumps information about currently tracked (potentially leaked) allocations.
 * Only available when DEBUG_BUDDY is defined.
 */
void buddy_dump_leaks(void);

// Macros to automatically capture file/line for debugging
#define BUDDY_ALLOC(size) buddy_alloc_internal(size, __FILE__, __LINE__)
#define BUDDY_FREE(ptr)   buddy_free_internal(ptr, __FILE__, __LINE__)

#else // !DEBUG_BUDDY

/**
 * @brief Allocates a block of memory of at least 'size' bytes.
 *
 * The actual allocated block will be a power of two size.
 *
 * @param size The minimum number of bytes required.
 * @return Pointer to the allocated memory block (aligned), or NULL if allocation fails.
 */
void *buddy_alloc(size_t size);

/**
 * @brief Frees a block of memory previously allocated by buddy_alloc.
 *
 * @param ptr Pointer to the memory block to free.
 * @param size The original size requested for allocation (used to determine the block order).
 * IMPORTANT: This MUST match the size used for allocation to ensure correct coalescing.
 * Using the debug version (BUDDY_FREE macro) is safer as it tracks the actual size.
 */
void buddy_free(void *ptr, size_t size); // Non-debug version still needs size for now

// Macros map directly to non-debug functions
#define BUDDY_ALLOC(size) buddy_alloc(size)
#define BUDDY_FREE(ptr, size) buddy_free(ptr, size) // Non-debug free requires size

#endif // DEBUG_BUDDY


/**
 * @brief Returns the amount of free memory currently available in the buddy system.
 *
 * @return Total free bytes.
 */
size_t buddy_free_space(void);


#ifdef __cplusplus
}
#endif

#endif // BUDDY_H