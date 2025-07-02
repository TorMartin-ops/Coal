/**
 * @file buddy_core.c
 * @brief Core buddy allocation algorithm following Single Responsibility Principle
 * 
 * This module is responsible ONLY for:
 * - Core buddy allocation and deallocation algorithms
 * - Free list management
 * - Block splitting and coalescing
 */

#define LOG_MODULE "buddy_core"

#include <kernel/memory/buddy_core.h>
#include <kernel/interfaces/logger.h>
#include <kernel/interfaces/memory_allocator.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>
#include <kernel/memory/kmalloc_internal.h>

// Configuration constants
#define MIN_ORDER 12  // 4KB minimum
#define MAX_ORDER 20  // 1MB maximum

// Helper macros
#ifndef ALIGN_DOWN
#define ALIGN_DOWN(addr, align) ((uintptr_t)(addr) & ~((uintptr_t)(align) - 1))
#endif
#define MIN_BLOCK_SIZE (1UL << MIN_ORDER)

// Free list structure
typedef struct buddy_block {
    struct buddy_block *next;
} buddy_block_t;

// Global state
static buddy_block_t *free_lists[MAX_ORDER + 1] = {0};
static uintptr_t heap_start_virt = 0;
static uintptr_t heap_end_virt = 0;
static size_t total_managed_size = 0;
static size_t free_bytes = 0;
static spinlock_t buddy_lock = {0};
static bool buddy_initialized = false;

/**
 * @brief Calculate the buddy address of a block
 */
static uintptr_t get_buddy_address(uintptr_t block_addr, uint8_t order) {
    size_t block_size = 1UL << order;
    uintptr_t relative_addr = block_addr - heap_start_virt;
    uintptr_t buddy_relative = relative_addr ^ block_size;
    return heap_start_virt + buddy_relative;
}

/**
 * @brief Check if an address is within the managed heap
 */
static bool is_valid_heap_address(uintptr_t addr) {
    return addr >= heap_start_virt && addr < heap_end_virt;
}

/**
 * @brief Calculate order needed for a given size
 */
static uint8_t size_to_order(size_t size) {
    if (size <= MIN_BLOCK_SIZE) {
        return MIN_ORDER;
    }
    
    uint8_t order = MIN_ORDER;
    size_t block_size = MIN_BLOCK_SIZE;
    
    while (block_size < size && order < MAX_ORDER) {
        order++;
        block_size <<= 1;
    }
    
    return order;
}

/**
 * @brief Add a block to the appropriate free list
 */
static void add_to_free_list(uintptr_t block_addr, uint8_t order) {
    KERNEL_ASSERT(order <= MAX_ORDER, "Invalid order for free list");
    KERNEL_ASSERT(is_valid_heap_address(block_addr), "Invalid block address");
    
    buddy_block_t *block = (buddy_block_t *)block_addr;
    block->next = free_lists[order];
    free_lists[order] = block;
    
    free_bytes += (1UL << order);
}

/**
 * @brief Remove a block from the free list
 */
static uintptr_t remove_from_free_list(uint8_t order) {
    if (order > MAX_ORDER || !free_lists[order]) {
        return 0;
    }
    
    buddy_block_t *block = free_lists[order];
    free_lists[order] = block->next;
    
    uintptr_t block_addr = (uintptr_t)block;
    free_bytes -= (1UL << order);
    
    return block_addr;
}

/**
 * @brief Remove a specific block from the free list
 */
static bool remove_specific_block(uintptr_t block_addr, uint8_t order) {
    if (order > MAX_ORDER) {
        return false;
    }
    
    buddy_block_t **current = &free_lists[order];
    
    while (*current) {
        if ((uintptr_t)(*current) == block_addr) {
            *current = (*current)->next;
            free_bytes -= (1UL << order);
            return true;
        }
        current = &(*current)->next;
    }
    
    return false;
}

/**
 * @brief Split a block into smaller blocks
 */
static void split_block(uintptr_t block_addr, uint8_t from_order, uint8_t to_order) {
    KERNEL_ASSERT(from_order > to_order, "Cannot split to larger order");
    
    uint8_t current_order = from_order;
    uintptr_t current_addr = block_addr;
    
    while (current_order > to_order) {
        current_order--;
        size_t half_size = 1UL << current_order;
        uintptr_t buddy_addr = current_addr + half_size;
        
        // Add the buddy half to the free list
        add_to_free_list(buddy_addr, current_order);
    }
}

/**
 * @brief Try to coalesce a block with its buddy
 */
static uintptr_t try_coalesce(uintptr_t block_addr, uint8_t order) {
    if (order >= MAX_ORDER) {
        return block_addr; // Cannot coalesce at max order
    }
    
    uintptr_t buddy_addr = get_buddy_address(block_addr, order);
    
    // Check if buddy is free
    if (!remove_specific_block(buddy_addr, order)) {
        return block_addr; // Buddy not free, cannot coalesce
    }
    
    // Coalesce - return the lower address
    uintptr_t coalesced_addr = (block_addr < buddy_addr) ? block_addr : buddy_addr;
    
    // Try to coalesce further up the chain
    return try_coalesce(coalesced_addr, order + 1);
}

/**
 * @brief Initialize buddy allocator core
 */
error_t buddy_core_init(uintptr_t start_virt, size_t size) {
    if (buddy_initialized) {
        LOGGER_WARN(LOG_MODULE, "Buddy allocator already initialized");
        return E_SUCCESS;
    }
    
    // Validate parameters
    if (start_virt == 0 || size == 0) {
        LOGGER_ERROR(LOG_MODULE, "Invalid initialization parameters");
        return E_INVAL;
    }
    
    // Align start address and size
    heap_start_virt = ALIGN_UP(start_virt, MIN_BLOCK_SIZE);
    total_managed_size = ALIGN_DOWN(size, MIN_BLOCK_SIZE);
    heap_end_virt = heap_start_virt + total_managed_size;
    
    // Initialize lock
    spinlock_init(&buddy_lock);
    
    // Clear free lists
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }
    
    // Add the entire heap as one large block (or multiple max-order blocks)
    uintptr_t current_addr = heap_start_virt;
    size_t remaining_size = total_managed_size;
    free_bytes = 0;
    
    while (remaining_size > 0) {
        // Find the largest block we can create
        uint8_t order = MAX_ORDER;
        size_t block_size = 1UL << order;
        
        while (block_size > remaining_size) {
            order--;
            block_size >>= 1;
        }
        
        // Add block to free list
        add_to_free_list(current_addr, order);
        
        current_addr += block_size;
        remaining_size -= block_size;
    }
    
    buddy_initialized = true;
    
    LOGGER_INFO(LOG_MODULE, "Buddy allocator initialized: start=%#lx, size=%zu, free=%zu",
               heap_start_virt, total_managed_size, free_bytes);
    
    return E_SUCCESS;
}

/**
 * @brief Allocate a block of memory
 */
uintptr_t buddy_core_alloc(size_t size) {
    if (!buddy_initialized || size == 0) {
        return 0;
    }
    
    uint8_t order = size_to_order(size);
    if (order > MAX_ORDER) {
        LOGGER_ERROR(LOG_MODULE, "Requested size %zu exceeds maximum", size);
        return 0;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&buddy_lock);
    
    // Find a free block of the required order or larger
    uint8_t alloc_order = order;
    while (alloc_order <= MAX_ORDER && !free_lists[alloc_order]) {
        alloc_order++;
    }
    
    if (alloc_order > MAX_ORDER) {
        // No suitable block found
        spinlock_release_irqrestore(&buddy_lock, flags);
        LOGGER_DEBUG(LOG_MODULE, "No free block available for size %zu", size);
        return 0;
    }
    
    // Remove block from free list
    uintptr_t block_addr = remove_from_free_list(alloc_order);
    
    // Split the block if necessary
    if (alloc_order > order) {
        split_block(block_addr, alloc_order, order);
    }
    
    spinlock_release_irqrestore(&buddy_lock, flags);
    
    LOGGER_DEBUG(LOG_MODULE, "Allocated block at %#lx, order %u for size %zu",
                block_addr, order, size);
    
    return block_addr;
}

/**
 * @brief Free a block of memory
 */
void buddy_core_free(uintptr_t block_addr, uint8_t order) {
    if (!buddy_initialized || block_addr == 0) {
        return;
    }
    
    if (!is_valid_heap_address(block_addr)) {
        LOGGER_ERROR(LOG_MODULE, "Invalid block address %#lx", block_addr);
        return;
    }
    
    if (order > MAX_ORDER) {
        LOGGER_ERROR(LOG_MODULE, "Invalid order %u", order);
        return;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&buddy_lock);
    
    // Try to coalesce with buddy
    uintptr_t coalesced_addr = try_coalesce(block_addr, order);
    uint8_t final_order = order;
    
    // Calculate final order after coalescing
    while (coalesced_addr != block_addr && final_order < MAX_ORDER) {
        final_order++;
        block_addr = coalesced_addr;
        coalesced_addr = try_coalesce(block_addr, final_order);
    }
    
    // Add the final block to free list
    add_to_free_list(block_addr, final_order);
    
    spinlock_release_irqrestore(&buddy_lock, flags);
    
    LOGGER_DEBUG(LOG_MODULE, "Freed block at %#lx, final order %u", block_addr, final_order);
}

/**
 * @brief Get allocator statistics
 */
void buddy_core_get_stats(size_t *total_size, size_t *free_size, size_t *used_size) {
    uintptr_t flags = spinlock_acquire_irqsave(&buddy_lock);
    
    if (total_size) *total_size = total_managed_size;
    if (free_size) *free_size = free_bytes;
    if (used_size) *used_size = total_managed_size - free_bytes;
    
    spinlock_release_irqrestore(&buddy_lock, flags);
}

/**
 * @brief Check if allocator is initialized
 */
bool buddy_core_is_initialized(void) {
    return buddy_initialized;
}

/**
 * @brief Cleanup buddy allocator
 */
void buddy_core_cleanup(void) {
    if (!buddy_initialized) {
        return;
    }
    
    // Clear all free lists
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }
    
    buddy_initialized = false;
    LOGGER_INFO(LOG_MODULE, "Buddy allocator cleaned up");
}