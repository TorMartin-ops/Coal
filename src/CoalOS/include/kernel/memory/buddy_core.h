/**
 * @file buddy_core.h
 * @brief Core buddy allocation algorithm interface
 */

#ifndef KERNEL_MEMORY_BUDDY_CORE_H
#define KERNEL_MEMORY_BUDDY_CORE_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize buddy allocator core
 * 
 * @param start_virt Virtual start address of heap
 * @param size Size of heap in bytes
 * @return E_SUCCESS on success, error code on failure
 */
error_t buddy_core_init(uintptr_t start_virt, size_t size);

/**
 * @brief Allocate a block of memory
 * 
 * @param size Size in bytes to allocate
 * @return Virtual address of allocated block, or 0 on failure
 */
uintptr_t buddy_core_alloc(size_t size);

/**
 * @brief Free a block of memory
 * 
 * @param block_addr Virtual address of block to free
 * @param order Order of the block (power of 2 exponent)
 */
void buddy_core_free(uintptr_t block_addr, uint8_t order);

/**
 * @brief Get allocator statistics
 * 
 * @param total_size Output total heap size (can be NULL)
 * @param free_size Output free bytes (can be NULL)
 * @param used_size Output used bytes (can be NULL)
 */
void buddy_core_get_stats(size_t *total_size, size_t *free_size, size_t *used_size);

/**
 * @brief Check if allocator is initialized
 * 
 * @return True if initialized, false otherwise
 */
bool buddy_core_is_initialized(void);

/**
 * @brief Cleanup buddy allocator
 */
void buddy_core_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_MEMORY_BUDDY_CORE_H