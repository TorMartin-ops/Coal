/**
 * @file buddy.h
 * @brief Physical page allocator (binary buddy system) with two zones.
 *
 * The span from the lowest to the highest usable address is managed in pages; ranges that are not handed
 * in with buddy_add_range() (holes, the kernel image, boot structures) stay reserved. Pages below 4 GiB
 * form the LOW zone (what 32-bit DMA engines can reach, used for all kernel objects); pages above it form
 * the HIGH zone (user data, taken first so the low memory lasts). Everything is reachable through the
 * direct map. Per-page state lives in an array of struct page, so free lists never touch the pages
 * themselves. Blocks are 2^order pages; the allocator remembers each block's order, so freeing needs only
 * the address.
 */
#ifndef BUDDY_H
#define BUDDY_H

#include <kernel/core/types.h>

#define BUDDY_MAX_ORDER 13                  /* largest block: 8192 pages = 32 MiB */

#define ZONE_LOW    0
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

/** Manage the span [base, end). `pages` must hold buddy_meta_bytes(end - base) bytes. Every page starts
 *  reserved: hand out memory with buddy_add_range(). */
void buddy_init(uintptr_t base, uintptr_t end, struct page *pages);

/** Make [start, end) (page aligned, inside the span) available for allocation. */
void buddy_add_range(uintptr_t start, uintptr_t end);

/** Allocate 2^order contiguous pages from the LOW zone. Returns the physical address or 0. */
uintptr_t buddy_alloc_pages(unsigned order);

/** Allocate one page, preferring the HIGH zone. */
uintptr_t buddy_alloc_page_user(void);

/** Free a block returned by one of the allocators. Panics on an invalid or double free. */
void buddy_free_pages(uintptr_t phys);

/** Page descriptor for a managed (not reserved) address, or NULL. */
struct page *buddy_page(uintptr_t phys);

size_t buddy_total_pages(unsigned zone);
size_t buddy_free_pages_count(unsigned zone);

#endif /* BUDDY_H */
