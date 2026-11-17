/**
 * @file buddy.h
 * @brief Physical page allocator (binary buddy system) with two zones.
 *
 * One contiguous RAM region is managed in pages. Pages below the lowmem limit
 * form the NORMAL zone (permanently direct-mapped, usable for kernel objects);
 * pages above it form the HIGH zone (user data only, reached through kmap()).
 * Per-page state lives in an array of struct page that the caller places in
 * lowmem, so free lists never touch the pages themselves. Blocks are 2^order
 * pages; the allocator remembers each block's order, so freeing needs only
 * the address.
 */
#ifndef BUDDY_H
#define BUDDY_H

#include <kernel/core/types.h>

#define BUDDY_MAX_ORDER 13                  /* largest block: 8192 pages = 32 MiB */

#define ZONE_NORMAL 0
#define ZONE_HIGH   1
#define ZONE_COUNT  2

struct page {
    uint32_t next, prev;                    /* free-list links (page indexes) */
    uint16_t refcount;                      /* frame.c: references by page-table entries */
    uint8_t  meta;                          /* free/allocated flag and block order */
};

/** Bytes of struct page needed to manage a region of the given size. */
static inline size_t buddy_meta_bytes(size_t region_bytes) {
    return (region_bytes >> 12) * sizeof(struct page);
}

/** Manage [base, end); pages from `high_start` up are the HIGH zone (high_start == end: none).
 *  `pages` (direct-mapped) must hold buddy_meta_bytes(end - base) bytes. */
void buddy_init(uintptr_t base, uintptr_t high_start, uintptr_t end, struct page *pages);

/** Allocate 2^order contiguous pages from the NORMAL zone. Returns the physical address or 0. */
uintptr_t buddy_alloc_pages(unsigned order);

/** Allocate one page, preferring the HIGH zone. */
uintptr_t buddy_alloc_page_user(void);

/** Free a block returned by one of the allocators. Panics on an invalid or double free. */
void buddy_free_pages(uintptr_t phys);

/** Page descriptor for a managed address, or NULL. */
struct page *buddy_page(uintptr_t phys);

size_t buddy_total_pages(unsigned zone);
size_t buddy_free_pages_count(unsigned zone);

#endif /* BUDDY_H */
