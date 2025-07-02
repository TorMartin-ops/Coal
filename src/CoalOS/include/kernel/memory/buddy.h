#ifndef BUDDY_H
#define BUDDY_H

#include <kernel/core/types.h> // Or stdint.h etc. - This should define size_t
// #include <stddef.h> // For size_t <-- REMOVE THIS LINE

// --- Configuration ---
#define MIN_ORDER 4   // Smallest block is 2^4 = 16 bytes (adjust as needed)
#define MAX_ORDER 23
// Example: If PAGE_SIZE is 4KB (2^12), max order might relate to page allocations.
#define MIN_BLOCK_SIZE (1 << MIN_ORDER)
// --- API ---


/**
 * @brief Initializes the buddy allocator system.
 * Must be called once before any allocations.
 * Assumes the provided memory region is virtually mapped and accessible.
 *
 * @param heap_region_start Virtual address of the start of the memory region.
 * @param region_size Size of the memory region in bytes.
 */
void buddy_init(void *heap_region_start, size_t region_size);

/**
 * @brief Allocates a block of memory of at least 'size' bytes.
 * The actual allocated block size will be a power of two.
 *
 * @param size The minimum number of bytes required.
 * @return Pointer to the allocated memory block, or NULL if allocation fails.
 * The returned pointer is aligned to DEFAULT_ALIGNMENT.
 */
void *buddy_alloc(size_t size);

/**
 * @brief Frees a previously allocated memory block.
 *
 * @param ptr Pointer to the memory block previously returned by buddy_alloc.
 * If ptr is NULL, the function does nothing.
 */
void buddy_free(void *ptr);


// --- Macros for Debug/Release ---
#ifdef DEBUG_BUDDY
    // Internal functions used by macros (or directly for tracker nodes)
    void *buddy_alloc_internal(size_t size, const char* file, int line);
    void buddy_free_internal(void *ptr, const char* file, int line);
    void buddy_dump_leaks(void); // Dumps unfreed allocations

    #define BUDDY_ALLOC(size) buddy_alloc_internal(size, __FILE__, __LINE__)
    #define BUDDY_FREE(ptr)   buddy_free_internal(ptr, __FILE__, __LINE__)
    #define BUDDY_DUMP_LEAKS() buddy_dump_leaks()

#else // Release version
    // Macros map directly to public API
    #define BUDDY_ALLOC(size) buddy_alloc(size)
    #define BUDDY_FREE(ptr)   buddy_free(ptr)
    #define BUDDY_DUMP_LEAKS() do { /* No-op in release */ } while(0)

#endif // DEBUG_BUDDY


// --- Statistics ---

typedef struct {
    size_t total_bytes;       // Total bytes managed by the allocator
    size_t free_bytes;        // Currently free bytes
    uint64_t alloc_count;     // Total successful allocations
    uint64_t free_count;      // Total frees
    uint64_t failed_alloc_count; // Total failed allocations
    // Add more detailed stats if needed (e.g., per-order counts)
} buddy_stats_t;

/**
 * @brief Gets the current amount of free space in the buddy allocator.
 * @return Number of free bytes.
 */
size_t buddy_free_space(void);

/**
 * @brief Gets the total amount of space managed by the buddy allocator.
 * @return Total number of managed bytes.
 */
size_t buddy_total_space(void);

/**
 * @brief Retrieves current statistics about the buddy allocator.
 * @param stats Pointer to a buddy_stats_t structure to fill.
 */
void buddy_get_stats(buddy_stats_t *stats);


void* buddy_alloc_raw(int order);

void buddy_free_raw(void* block_addr_virt, int order);




#endif // BUDDY_H