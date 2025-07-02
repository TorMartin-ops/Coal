/**
 * @file paging_temp.h
 * @brief Temporary virtual address mapping management
 */

#ifndef COAL_MEMORY_PAGING_TEMP_H
#define COAL_MEMORY_PAGING_TEMP_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief Initialize the temporary mapping system
 * @return 0 on success, negative error code on failure
 */
int paging_temp_map_init(void);

/**
 * @brief Temporarily map a physical page to a virtual address
 * @param phys_addr Physical address to map (must be page-aligned)
 * @return Virtual address of mapping or NULL on failure
 */
void* paging_temp_map(uintptr_t phys_addr);

/**
 * @brief Unmap a temporary virtual address
 * @param temp_vaddr Virtual address returned by paging_temp_map (as uintptr_t)
 */
void paging_temp_unmap(uintptr_t temp_vaddr);

/**
 * @brief Get statistics about temporary mappings
 * @param total_slots Output total number of slots
 * @param used_slots Output number of used slots
 */
void paging_temp_get_stats(size_t* total_slots, size_t* used_slots);

#endif // COAL_MEMORY_PAGING_TEMP_H