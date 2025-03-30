#include "buddy.h"
#include "terminal.h"  // For debug output; disable or remove in production

#include "libc/stddef.h"    // Provides size_t and NULL
#include "libc/stdbool.h"   // Provides bool, true, false
#include "libc/stdint.h"    // Intended to provide uint32_t, etc.

// Ensure uintptr_t is defined. If not, define it for i386.
#ifndef UINTPTR_MAX
typedef unsigned int uintptr_t;
#endif

// Use a common page size definition for integration with paging and slab allocators.
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// Define the minimum order (MIN_ORDER = 5 means minimum block size is 2^5 = 32 bytes)
#define MIN_ORDER 5

// Define the maximum order such that 2^(MAX_ORDER) equals the maximum block size.
// For example, MAX_ORDER = 21 gives blocks up to 2 MB (if MIN_ORDER = 5).
#define MAX_ORDER 21

// Minimum block size in bytes.
#define MIN_BLOCK_SIZE (1 << MIN_ORDER)

// ----------------------------------------------------------------------------
// Data Structures and Global Variables
// ----------------------------------------------------------------------------

// Each free block is represented by a simple structure containing a pointer to the next free block.
// The actual block size is implicit in the order (i.e. block size = 2^(order)).
typedef struct buddy_block {
    struct buddy_block *next;
} buddy_block_t;

// An array of free lists; each index corresponds to an order from MIN_ORDER to MAX_ORDER.
// For example, free_lists[order] contains blocks of size 2^(order) bytes.
static buddy_block_t *free_lists[MAX_ORDER + 1] = {0};

// ----------------------------------------------------------------------------
// Internal Helper Functions
// ----------------------------------------------------------------------------

/**
 * next_power_of_two
 *
 * Rounds up the given size to the next power of two, ensuring a minimum of MIN_BLOCK_SIZE.
 */
static size_t next_power_of_two(size_t size) {
    size_t power = MIN_BLOCK_SIZE;
    while (power < size)
        power <<= 1;
    return power;
}

/**
 * size_to_order
 *
 * Computes the order corresponding to a block size.
 * For example, if MIN_BLOCK_SIZE is 32 (2^5), then a 32-byte block is order 5.
 */
static int size_to_order(size_t size) {
    int order = MIN_ORDER;
    size_t block_size = MIN_BLOCK_SIZE;
    while (block_size < size) {
        block_size <<= 1;
        order++;
    }
    return order;
}

// ----------------------------------------------------------------------------
// Public API Functions
// ----------------------------------------------------------------------------

void buddy_init(void *heap, size_t size) {
    // Assume that 'size' is exactly a power of two.
    int order = size_to_order(size);
    if (order > MAX_ORDER) {
        terminal_write("Error: Heap size too large for buddy system.\n");
        return;
    }
    // Initialize all free lists (for orders MIN_ORDER to MAX_ORDER) to NULL.
    for (int i = MIN_ORDER; i <= MAX_ORDER; i++) {
        free_lists[i] = 0;
    }
    // Insert the entire heap as a single free block at the computed order.
    buddy_block_t *block = (buddy_block_t *)heap;
    block->next = NULL;
    free_lists[order] = block;
    terminal_write("Buddy allocator initialized.\n");
}

void *buddy_alloc(size_t size) {
    if (size == 0)
        return NULL;
    
    // Round up the requested size to the next power of two.
    size_t req_size = next_power_of_two(size);
    int req_order = size_to_order(req_size);
    
    // Search for a free block in the free lists of order req_order or higher.
    int order = req_order;
    while (order <= MAX_ORDER && free_lists[order] == NULL)
        order++;
    
    if (order > MAX_ORDER) {
        terminal_write("Buddy_alloc: Out of memory.\n");
        return NULL;
    }
    
    // Remove a block from the free list.
    buddy_block_t *block = free_lists[order];
    free_lists[order] = block->next;
    
    // Split the block repeatedly until we reach the desired order.
    while (order > req_order) {
        order--;
        size_t block_size = (size_t)1 << order;
        // Split block into two buddies.
        buddy_block_t *buddy = (buddy_block_t *)((uint8_t *)block + block_size);
        // Insert the buddy into the free list at the lower order.
        buddy->next = free_lists[order];
        free_lists[order] = buddy;
    }
    
    return (void *)block;
}

void buddy_free(void *ptr, size_t size) {
    if (!ptr)
        return;
    
    size_t req_size = next_power_of_two(size);
    int order = size_to_order(req_size);
    
    // Convert pointer to an integer address.
    uintptr_t addr = (uintptr_t)ptr;
    
    // Attempt to merge the freed block with its buddy recursively.
    while (order < MAX_ORDER) {
        uintptr_t buddy_addr = addr ^ ((uintptr_t)1 << order);
        buddy_block_t **prev = &free_lists[order];
        buddy_block_t *curr = free_lists[order];
        bool merged = false;
        while (curr) {
            if ((uintptr_t)curr == buddy_addr) {
                // Buddy found; remove it from the free list.
                *prev = curr->next;
                // Update addr to the lower of the two addresses.
                if (buddy_addr < addr)
                    addr = buddy_addr;
                order++;
                merged = true;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }
        if (!merged)
            break;
    }
    
    // Insert the (possibly merged) block back into the free list for the final order.
    buddy_block_t *block = (buddy_block_t *)addr;
    block->next = free_lists[order];
    free_lists[order] = block;
}
