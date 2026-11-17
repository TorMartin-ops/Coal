/**
 * @file paging_early.h
 * @brief Boot-time memory bring-up.
 */
#ifndef PAGING_EARLY_H
#define PAGING_EARLY_H

#include <kernel/core/types.h>

/**
 * Bring up all of memory management while paging is still off.
 *
 * Builds the kernel page directory (identity map, direct map of lowmem,
 * pre-allocated kernel stack and kmap tables), enables paging, then starts the
 * page allocator on whatever part of the region the boot structures did not
 * use, and finally kmalloc. Halts the machine on failure.
 *
 * @param heap_phys  start of the largest usable RAM region (not overlapping the
 *                   kernel image or the boot info)
 * @param heap_size  its size in bytes; memory above 896 MiB becomes highmem and
 *                   the region is clipped at 4 GiB
 */
void memory_init(uintptr_t heap_phys, size_t heap_size);

#endif /* PAGING_EARLY_H */
