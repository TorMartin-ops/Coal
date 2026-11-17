/**
 * @file kmap.h
 * @brief Temporary kernel mappings for highmem frames.
 *
 * Lowmem frames are already direct-mapped, so kmap() returns their address
 * at no cost. A highmem frame gets one of a few fixed PTEs for the duration
 * of the access. Slots are never held across a sleep, so a single pool with
 * interrupts masked while allocating is enough on this single-CPU kernel
 * (index the pool by CPU when SMP arrives).
 */
#ifndef KMAP_H
#define KMAP_H

#include <kernel/core/types.h>

#define KMAP_SLOTS 4

/** Map a frame (any physical address below 4 GiB) for CPU access. Panics if all slots are in use. */
void *kmap(uintptr_t phys);

/** Release a mapping obtained from kmap(). */
void kunmap(void *vaddr);

#endif /* KMAP_H */
