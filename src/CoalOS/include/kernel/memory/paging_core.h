/**
 * @file paging_core.h
 * @brief Core paging operations
 */

#ifndef COAL_MEMORY_PAGING_CORE_H
#define COAL_MEMORY_PAGING_CORE_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief Map a range of physical memory to virtual addresses
 * @param page_directory_phys Physical address of page directory
 * @param virt_start_addr Virtual start address (must be page-aligned)
 * @param phys_start_addr Physical start address (must be page-aligned)
 * @param memsz Size of memory range (will be rounded up to page size)
 * @param flags Page table entry flags
 * @return 0 on success, negative error code on failure
 */
int paging_map_range(uint32_t* page_directory_phys, 
                     uintptr_t virt_start_addr,
                     uintptr_t phys_start_addr, 
                     size_t memsz, 
                     uint32_t flags);

/**
 * @brief Unmap a range of virtual addresses
 * @param page_directory_phys Physical address of page directory
 * @param virt_start_addr Virtual start address (must be page-aligned)
 * @param memsz Size of memory range (will be rounded up to page size)
 * @return 0 on success, negative error code on failure
 */
int paging_unmap_range(uint32_t* page_directory_phys,
                       uintptr_t virt_start_addr,
                       size_t memsz);

/**
 * @brief Map a single 4KB page
 * @param page_directory_phys Physical address of page directory
 * @param vaddr Virtual address (must be page-aligned)
 * @param paddr Physical address (must be page-aligned)
 * @param flags Page table entry flags
 * @return 0 on success, negative error code on failure
 */
int paging_map_single_4k(uint32_t* page_directory_phys,
                         uintptr_t vaddr,
                         uintptr_t paddr,
                         uint32_t flags);

/**
 * @brief Get physical address for a virtual address
 * @param page_directory_phys Physical address of page directory
 * @param vaddr Virtual address to look up
 * @param paddr Output physical address
 * @return 0 on success, negative error code on failure
 */
int paging_get_physical_address(uint32_t* page_directory_phys,
                                uintptr_t vaddr,
                                uintptr_t* paddr);

/**
 * @brief Get physical address and flags for a virtual address
 * @param page_directory_phys Physical address of page directory
 * @param vaddr Virtual address to look up
 * @param paddr_out Output physical address
 * @param flags_out Output page flags
 * @return 0 on success, negative error code on failure
 */
int paging_get_physical_address_and_flags(uint32_t* page_directory_phys,
                                          uintptr_t vaddr,
                                          uintptr_t* paddr_out,
                                          uint32_t* flags_out);

/**
 * @brief Invalidate TLB entry for a specific page
 * @param vaddr Virtual address of page to invalidate
 */
void paging_invalidate_page(void* vaddr);

/**
 * @brief Flush TLB entries for a range of addresses
 * @param start Start virtual address
 * @param size Size of range
 */
void tlb_flush_range(void* start, size_t size);

/**
 * @brief Set kernel page directory addresses
 * @param pd_virt Virtual address of kernel page directory
 * @param pd_phys Physical address of kernel page directory
 */
void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys);

#endif // COAL_MEMORY_PAGING_CORE_H