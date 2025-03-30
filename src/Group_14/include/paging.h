#pragma once
#ifndef PAGING_H
#define PAGING_H

#include "libc/stdint.h"
#include "libc/stddef.h"

/* Paging flags */
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

/**
 * @brief Sets the global page directory pointer.
 *
 * @param pd A pointer to the page directory.
 */
void paging_set_directory(uint32_t* pd);

/**
 * @brief Maps a single virtual address to an identity mapping.
 *
 * If no page table exists for the corresponding directory entry, a new page table
 * is allocated using the buddy allocator.
 *
 * @param virt_addr The virtual address to map.
 */
void paging_map_page(void* virt_addr);

/**
 * @brief Maps a contiguous range of virtual addresses to identity mappings.
 *
 * This function maps 'memsz' bytes starting from 'virt_addr' using the given flags.
 * It allocates new page tables dynamically if needed.
 *
 * @param page_directory The page directory to update.
 * @param virt_addr      Starting virtual address.
 * @param memsz          Size of the region to map (in bytes).
 * @param flags          Paging flags (e.g., PAGE_PRESENT | PAGE_RW).
 * @return 0 on success, non-zero on failure.
 */
int paging_map_range(uint32_t *page_directory, uint32_t virt_addr, uint32_t memsz, uint32_t flags);

#endif // PAGING_H
