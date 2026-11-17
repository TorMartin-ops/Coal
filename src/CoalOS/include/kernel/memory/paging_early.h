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
 * Builds the kernel page directory (identity map, direct map of the heap,
 * pre-allocated kernel stack tables), enables paging, then starts the frame
 * allocator on whatever part of the heap the boot page tables did not use,
 * and finally kmalloc. Halts the machine on failure.
 *
 * @param heap_phys  start of the largest usable RAM region (not overlapping the
 *                   kernel image or the boot info)
 * @param heap_size  its size in bytes (at most 256 MiB is used)
 */
void memory_init(uintptr_t heap_phys, size_t heap_size);

/** No-op kept for older callers: temporary mappings are not needed any more. */
static inline int paging_temp_map_init(void) { return 0; }

#endif /* PAGING_EARLY_H */
