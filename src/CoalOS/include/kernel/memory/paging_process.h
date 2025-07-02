/**
 * @file paging_process.h
 * @brief Process-specific paging operations
 */

#ifndef COAL_MEMORY_PAGING_PROCESS_H
#define COAL_MEMORY_PAGING_PROCESS_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief Clone a page directory for a new process
 * @param src_pd_phys Physical address of source page directory
 * @return Physical address of new page directory or 0 on failure
 */
uintptr_t paging_clone_directory(uint32_t* src_pd_phys);

/**
 * @brief Free all user space mappings in a page directory
 * @param page_directory_phys Physical address of page directory
 */
void paging_free_user_space(uint32_t* page_directory_phys);

/**
 * @brief Copy kernel page directory entries to a new page directory
 * @param new_pd_virt Virtual address of new page directory
 */
void copy_kernel_pde_entries(uint32_t* new_pd_virt);

/**
 * @brief Switch to a different page directory
 * @param pd_phys Physical address of page directory
 */
void paging_switch_directory(uint32_t pd_phys);

/**
 * @brief Get current page directory physical address
 * @return Physical address of current page directory
 */
uint32_t paging_get_current_directory(void);

#endif // COAL_MEMORY_PAGING_PROCESS_H