#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Common flags
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

/**
 * Sets the global kernel_page_directory pointer (if you track it in paging.c).
 */
void paging_set_directory(uint32_t* pd);

/**
 * paging_map_page
 *
 * Maps a single virtual address to an identity mapping in the global PD.
 * If PDE is missing, allocate from buddy. 
 */
void paging_map_page(void* virt_addr);

/**
 * paging_map_range
 *
 * Maps [virt_addr..virt_addr+memsz) to identity with 'flags',
 * allocating page tables if needed.
 * 
 * @param page_directory The PD to modify
 * @param virt_addr      start address
 * @param memsz          size in bytes
 * @param flags          PAGE_PRESENT | PAGE_RW | PAGE_USER, etc.
 * @return 0 on success, -1 on failure
 */
int paging_map_range(uint32_t *page_directory, uint32_t virt_addr, 
                     uint32_t memsz, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif // PAGING_H
