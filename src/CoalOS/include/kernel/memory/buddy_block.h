/**
 * @file buddy_block.h
 * @brief Buddy Allocator Block Management Interface
 * 
 * Provides low-level block operations for the buddy allocator.
 */

#ifndef KERNEL_MEMORY_BUDDY_BLOCK_H
#define KERNEL_MEMORY_BUDDY_BLOCK_H

#include <libc/stdint.h>
#include <libc/stdbool.h>
#include <libc/stddef.h>

// Configuration constants (from buddy.h)
#ifndef MIN_ORDER
#define MIN_ORDER 12  // 4KB minimum allocation
#endif

#ifndef MAX_ORDER
#define MAX_ORDER 22  // 4MB maximum allocation
#endif

// Block header structure
typedef struct buddy_block {
    struct buddy_block *next;    // Next block in free list
    struct buddy_block *prev;    // Previous block in free list
    uint8_t order;              // Order of this block (log2 of size)
    uint8_t is_free;            // 1 if free, 0 if allocated
    uint8_t reserved[2];        // Padding for alignment
} buddy_block_t;

/**
 * @brief Calculate the buddy address of a block
 * @param block_addr Virtual address of the block
 * @param order Order of the block
 * @return Virtual address of the buddy block
 */
uintptr_t buddy_block_get_buddy_addr(uintptr_t block_addr, int order);

/**
 * @brief Add a block to the appropriate free list
 * @param block_ptr Pointer to the block
 * @param order Order of the block
 */
void buddy_block_add_to_free_list(void *block_ptr, int order);

/**
 * @brief Remove a block from its free list
 * @param block_ptr Pointer to the block
 * @param order Order of the block
 * @return true if block was found and removed, false otherwise
 */
bool buddy_block_remove_from_free_list(void *block_ptr, int order);

/**
 * @brief Find a free block in the free lists
 * @param order Minimum order needed
 * @return Pointer to a free block, or NULL if none found
 */
buddy_block_t* buddy_block_find_free_block(int order);

/**
 * @brief Split a block into two buddies
 * @param block Block to split
 * @param from_order Current order of the block
 * @param to_order Target order after splitting
 * @return Pointer to the first half after splitting
 */
buddy_block_t* buddy_block_split(buddy_block_t *block, int from_order, int to_order);

/**
 * @brief Attempt to coalesce a block with its buddy
 * @param block_addr Address of the block to coalesce
 * @param order Current order of the block
 * @return New order after coalescing (may be same if no coalescing occurred)
 */
int buddy_block_coalesce(uintptr_t block_addr, int order);

/**
 * @brief Initialize block headers in a memory region
 * @param start_addr Starting address
 * @param end_addr Ending address (exclusive)
 * @param order Order for all blocks in the region
 */
void buddy_block_init_region(uintptr_t start_addr, uintptr_t end_addr, int order);

/**
 * @brief Validate a block's header
 * @param block Block to validate
 * @return true if block appears valid, false otherwise
 */
bool buddy_block_validate(buddy_block_t *block);

#endif // KERNEL_MEMORY_BUDDY_BLOCK_H