/**
 * @file frame.h
 * @brief Reference-counted physical frames (single 4 KiB pages).
 *
 * Frames come from the buddy allocator and lie in the kernel direct map
 * (see paging.h). A present page-table entry owns one reference; unmapping
 * drops it. Frame contents are not cleared unless asked for.
 */
#ifndef FRAME_H
#define FRAME_H

#include <kernel/core/types.h>

/** Allocate the reference-count array. Called once after buddy_init(). */
void frame_init(void);

/** Allocate a frame with reference count 1. Returns its physical address, or 0 when out of memory. */
uintptr_t frame_alloc(void);

/** Same, with the frame zero-filled. */
uintptr_t frame_alloc_zeroed(void);

/** Take an additional reference. */
void get_frame(uintptr_t phys);

/** Drop a reference; the frame is freed when the last one goes. Addresses
 *  outside the allocator's memory (MMIO, boot memory) are ignored. Panics on
 *  an unreferenced frame. */
void put_frame(uintptr_t phys);

static inline void frame_free(uintptr_t phys) { put_frame(phys); }

/** Current reference count, or -1 for an address the allocator does not manage. */
int get_frame_refcount(uintptr_t phys);

/** Total and free page counts of the allocator. */
void frame_stats(size_t *total_pages, size_t *free_pages);

#endif /* FRAME_H */
