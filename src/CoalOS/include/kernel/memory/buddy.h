/**
 * @file buddy.h
 * @brief Physical page allocator (binary buddy system).
 *
 * Manages one contiguous RAM region in units of pages (its per-page metadata
 * is stored at the start of the region). Blocks are 2^order
 * pages; a block's order is remembered by the allocator, so freeing needs only
 * the address. All managed memory must lie inside the kernel direct map.
 */
#ifndef BUDDY_H
#define BUDDY_H

#include <kernel/core/types.h>

#define BUDDY_MAX_ORDER 13                  /* largest block: 8192 pages = 32 MiB */
#define BUDDY_MAX_PAGES 65536u              /* managed region is capped at 256 MiB */

/** Start managing [phys_start, phys_start+size). Paging must be active. */
void buddy_init(uintptr_t phys_start, size_t size);

/** Allocate 2^order contiguous pages. Returns the physical address or 0. */
uintptr_t buddy_alloc_pages(unsigned order);

/** Free a block returned by buddy_alloc_pages(). Panics on an invalid or double free. */
void buddy_free_pages(uintptr_t phys);

/** Page index of phys inside the managed region, or -1 if it is not managed. */
long buddy_page_index(uintptr_t phys);

size_t buddy_total_pages(void);
size_t buddy_free_pages_count(void);

#endif /* BUDDY_H */
