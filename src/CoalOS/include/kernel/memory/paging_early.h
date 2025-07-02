/**
 * @file paging_early.h
 * @brief Early boot paging initialization
 */

#ifndef COAL_MEMORY_PAGING_EARLY_H
#define COAL_MEMORY_PAGING_EARLY_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief Initialize a new page directory for early boot
 * @param out_pd_phys Output physical address of the page directory
 * @return 0 on success, negative error code on failure
 */
int paging_initialize_directory(uintptr_t* out_pd_phys);

/**
 * @brief Set up early memory mappings (identity, kernel, heap)
 * @param page_directory_phys Physical address of page directory
 * @param kernel_phys_start Physical start of kernel
 * @param kernel_phys_end Physical end of kernel
 * @param heap_phys_start Physical start of heap
 * @param heap_size Size of heap
 * @return 0 on success, negative error code on failure
 */
int paging_setup_early_maps(uintptr_t page_directory_phys,
                            uintptr_t kernel_phys_start, 
                            uintptr_t kernel_phys_end,
                            uintptr_t heap_phys_start, 
                            size_t heap_size);

/**
 * @brief Finalize paging setup and activate
 * @param page_directory_phys Physical address of page directory
 * @param total_memory_bytes Total system memory
 * @return 0 on success, negative error code on failure
 */
int paging_finalize_and_activate(uintptr_t page_directory_phys, 
                                 uintptr_t total_memory_bytes);

/**
 * @brief Allocate a physical frame during early boot
 * @return Physical address of allocated frame or 0 on failure
 */
uintptr_t paging_alloc_early_frame_physical(void);

/**
 * @brief Check and enable PSE (Page Size Extension) if supported
 * @return true if PSE is supported and enabled
 */
bool check_and_enable_pse(void);

/**
 * @brief Check and enable NX (No-Execute) bit if supported
 * @return true if NX is supported and enabled
 */
bool check_and_enable_nx(void);

#endif // COAL_MEMORY_PAGING_EARLY_H