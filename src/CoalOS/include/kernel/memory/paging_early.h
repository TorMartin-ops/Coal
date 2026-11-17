/**
 * @file paging_early.h
 * @brief Boot-time memory bring-up.
 */
#ifndef PAGING_EARLY_H
#define PAGING_EARLY_H

#include <kernel/core/types.h>
#include <kernel/core/boot_info.h>

/**
 * Bring up all of memory management on the boot page tables.
 *
 * Builds the kernel PML4 (direct map of every RAM range, the kernel image with W^X permissions, the shared
 * upper tables for the stack and device windows) and switches to it, which drops the identity map the boot
 * code set up. Then starts the page allocator on the memory the early allocations left over, and kmalloc.
 * Halts the machine on failure.
 *
 * @param map  usable RAM, page aligned, without the kernel image and the boot information
 */
void memory_init(const struct boot_memmap *map);

#endif /* PAGING_EARLY_H */
