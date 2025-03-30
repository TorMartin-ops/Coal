#ifndef PAGING_H
#define PAGING_H

#include <libc/stdint.h>
#include <libc/stddef.h>

/* Paging flags */
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

/**
 * Sets the global page directory pointer.
 *
 * @param pd A pointer to the page directory.
 */
void paging_set_directory(uint32_t* pd);

/**
 * Maps the virtual address 'virt_addr' to an identity mapping.
 * If no page table exists for the directory entry, a new one is allocated
 * from a fixed pool.
 *
 * @param virt_addr The virtual address to map.
 */
void paging_map_page(void* virt_addr);

#endif // PAGING_H
