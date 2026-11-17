/**
 * @file frame.h
 * @brief Reference-counted physical frames (single 4 KiB pages).
 *
 * frame_alloc*() return lowmem frames, always reachable through the direct
 * map (page tables, kernel stacks, anything the kernel dereferences).
 * frame_alloc_user*() may return highmem frames, which are for user data only
 * and must be touched through kmap(). A present page-table entry owns one
 * reference; unmapping drops it.
 */
#ifndef FRAME_H
#define FRAME_H

#include <kernel/core/types.h>

/** Lowmem frame with reference count 1, contents undefined. Returns its physical address, or 0 when out of memory. */
uintptr_t frame_alloc(void);

/** Same, zero-filled (from the pre-zeroed pool when it has one). */
uintptr_t frame_alloc_zeroed(void);

/** User-data frame (highmem preferred), contents undefined. */
uintptr_t frame_alloc_user(void);

/** Same, zero-filled. */
uintptr_t frame_alloc_user_zeroed(void);

/** Idle-time work: zero one more frame for the pool that frame_alloc*_zeroed() draws from.
 *  Returns false when the pool is full or memory is short, true if it did something. */
bool frame_refill_zeroed(void);

/** Frames waiting in the zeroed pool in a zone (they count as free memory). */
size_t frame_pool_pages(unsigned zone);

/** Copy one frame to another; either may be highmem. */
void frame_copy(uintptr_t dst, uintptr_t src);

/** Take an additional reference. */
void get_frame(uintptr_t phys);

/** Drop a reference; the frame is freed when the last one goes. Addresses
 *  outside the allocator's memory (MMIO, boot memory) are ignored. Panics on
 *  an unreferenced frame. */
void put_frame(uintptr_t phys);

static inline void frame_free(uintptr_t phys) { put_frame(phys); }

/** Current reference count, or -1 for an address the allocator does not manage. */
int get_frame_refcount(uintptr_t phys);

#endif /* FRAME_H */
