#include "buddy.h"
#include "terminal.h"  // For debug prints (disable in production)
#include "types.h"

#ifndef UINTPTR_MAX
typedef unsigned int uintptr_t;
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// The minimum order: 2^5 = 32 bytes
#define MIN_ORDER 5

// The maximum order. Example: 21 => 2^(21) = 2MB
#define MAX_ORDER 21

// The smallest block size is 2^(MIN_ORDER) = 32
#define MIN_BLOCK_SIZE (1 << MIN_ORDER)

/**
 * Each free block is a linked list node. The block size is implied by its "order".
 * The buddy system is organized so that free_lists[order] contains blocks of size 2^(order).
 */
typedef struct buddy_block {
    struct buddy_block *next;
} buddy_block_t;

/* 
 * Global array of free lists, from MIN_ORDER..MAX_ORDER. 
 * free_lists[i] is a linked list of free blocks of size 2^i. 
 */
static buddy_block_t *free_lists[MAX_ORDER + 1] = {0};

/* 
 * Track the total size of the buddy-managed region and how many bytes remain free. 
 * These are optional “debugging” variables to help you monitor usage.
 */
static size_t g_buddy_total_size = 0;
static size_t g_buddy_free_bytes = 0;

/**
 * buddy_round_up_to_power_of_two
 * 
 * Rounds an integer size up to the next power of two, 
 * ensuring it's at least MIN_BLOCK_SIZE.
 */
static size_t buddy_round_up_to_power_of_two(size_t size) {
    size_t power = MIN_BLOCK_SIZE;
    while (power < size)
        power <<= 1;
    return power;
}

/**
 * buddy_size_to_order
 *
 * Determines which “order” matches a given size. 
 * For instance, if MIN_ORDER=5 (32 bytes) and size=128, 
 * then the order is 7 (2^7=128).
 */
static int buddy_size_to_order(size_t size) {
    int order = MIN_ORDER;
    size_t block_size = (size_t)1 << MIN_ORDER;  // e.g. 32
    while (block_size < size) {
        block_size <<= 1;
        order++;
    }
    return order;
}

/**
 * buddy_init
 *
 * Initializes the buddy system over a heap region [heap, heap + size).
 * - 'size' should be a power of two (or at least big enough to hold a block).
 * - All memory is initially inserted as one large free block at order=buddy_size_to_order(size).
 *
 * @param heap  Start of the free memory region.
 * @param size  The total size in bytes, ideally a power of two.
 */
void buddy_init(void *heap, size_t size) {
    // Wipe all free lists
    for (int i = MIN_ORDER; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }

    // Round the region size up if it's not a perfect power of two
    // or at least handle the largest power-of-two that fits.
    // But we keep the original approach: "assume exact power of two".
    int order = buddy_size_to_order(size);
    if (order > MAX_ORDER) {
        terminal_write("[Buddy] Error: heap size too large for buddy.\n");
        return;
    }

    // We store debug usage stats
    g_buddy_total_size = (size_t)1 << order; // The actual block size used
    g_buddy_free_bytes = g_buddy_total_size;

    // Put a single free block in the free list at 'order'.
    buddy_block_t *block = (buddy_block_t*)heap;
    block->next = NULL;
    free_lists[order] = block;

    terminal_write("[Buddy] init done. size=");
    // Print numeric
    // (Quick & dirty decimal printing or just say in hex)
    terminal_write("0x");
    // We'll do a quick hex print
    {
        char buf[9];
        unsigned val = g_buddy_total_size;
        for (int i = 0; i < 8; i++) {
            unsigned nibble = (val >> ((7-i)*4)) & 0xF;
            buf[i] = (nibble < 10) ? ('0'+nibble) : ('A'+nibble-10);
        }
        buf[8] = '\0';
        terminal_write(buf);
    }
    terminal_write(" bytes\n");
}

/**
 * buddy_alloc
 *
 * Allocates 'size' bytes from the buddy system. 
 * The allocation is rounded up to the next power of two. 
 * Then we look for a free block in that order or higher. 
 * If found in a higher order, we split blocks down until we get the desired size.
 *
 * @param size Number of bytes requested.
 * @return A pointer to the allocated block, or NULL if out of memory.
 */
void *buddy_alloc(size_t size) {
    if (size == 0) return NULL;

    // Round to next power of two
    size_t req_size = buddy_round_up_to_power_of_two(size);
    int req_order   = buddy_size_to_order(req_size);

    // Find a free block at order req_order or higher
    int order = req_order;
    while (order <= MAX_ORDER && free_lists[order] == NULL) {
        order++;
    }
    if (order > MAX_ORDER) {
        terminal_write("[Buddy] Out of memory (no block >= requested size)\n");
        return NULL;
    }

    // Pop one block from free_lists[order].
    buddy_block_t *block = free_lists[order];
    free_lists[order]     = block->next;

    // "Split" bigger block down to the exact order we need
    while (order > req_order) {
        order--;
        size_t half_size = (size_t)1 << order;
        // The second half is the "buddy"
        buddy_block_t *buddy = (buddy_block_t*)((uint8_t*)block + half_size);
        // Insert the buddy in the free list of [order].
        buddy->next = free_lists[order];
        free_lists[order] = buddy;
    }

    // Decrease free usage
    g_buddy_free_bytes -= req_size;

    return (void*)block;
}

/**
 * buddy_free
 *
 * Frees a previously allocated block of 'size' bytes. 
 * The buddy system merges the block with its buddy if possible, 
 * repeating until we can no longer merge into a bigger block.
 *
 * @param ptr  The pointer returned by buddy_alloc.
 * @param size The original requested size (the buddy system will round it).
 */
void buddy_free(void *ptr, size_t size) {
    if (!ptr) return;

    // Round size up to the actual block size we allocated
    size_t req_size = buddy_round_up_to_power_of_two(size);
    int order = buddy_size_to_order(req_size);

    // Reclaim usage
    g_buddy_free_bytes += req_size;

    // We repeatedly check if its buddy is free to merge
    uintptr_t addr = (uintptr_t)ptr;

    while (order < MAX_ORDER) {
        // Buddy address is flipping the bit at the current order
        uintptr_t buddy_addr = addr ^ ((uintptr_t)1 << order);

        // See if buddy_addr is on the free list of the same 'order'
        buddy_block_t *prev = NULL;
        buddy_block_t *curr = free_lists[order];
        bool merged = false;
        while (curr) {
            if ((uintptr_t)curr == buddy_addr) {
                // Remove the buddy from the free list
                if (prev) 
                    prev->next = curr->next;
                else
                    free_lists[order] = curr->next;

                // Merge
                if (buddy_addr < addr)
                    addr = buddy_addr;
                order++;
                merged = true;
                break;
            }
            prev = curr;
            curr = curr->next;
        }
        if (!merged) {
            break;
        }
    }

    // Insert the final block back into the free list of 'order'
    buddy_block_t *block = (buddy_block_t*)addr;
    block->next = free_lists[order];
    free_lists[order] = block;
}

/**
 * buddy_free_space
 *
 * Returns how many bytes are free in the buddy system (for debugging).
 */
size_t buddy_free_space(void) {
    return g_buddy_free_bytes;
}
