/**
 * @file buddy.c
 * @brief Binary buddy page allocator with a LOW (below 4 GiB) and a HIGH zone.
 *
 * Free blocks sit on doubly linked lists threaded through struct page by page
 * index, so removing a buddy while merging is O(1). A block never merges across the zone boundary
 * (4 GiB is aligned to the largest block) or over reserved pages.
 */

#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>

#define META_FREE  0x80u
#define META_ALLOC 0x40u
#define META_RESERVED 0x20u
#define META_ORDER 0x1Fu
#define NONE       0xFFFFFFFFu

static uint32_t g_free[ZONE_COUNT][BUDDY_MAX_ORDER + 1];
static struct page *g_pages;
static uintptr_t g_base;
static size_t g_count;                  /* pages in the span */
static size_t g_zone_pages[ZONE_COUNT];
static size_t g_zone_free[ZONE_COUNT];
static spinlock_t g_lock;

static inline unsigned zone_of(size_t idx) { return g_base + ((uintptr_t)idx << PAGE_SHIFT) >= ZONE_HIGH_START_PHYS ? ZONE_HIGH : ZONE_LOW; }

static void push_free(size_t idx, unsigned order) {
    uint32_t *head = &g_free[zone_of(idx)][order];
    g_pages[idx].prev = NONE;
    g_pages[idx].next = *head;
    if (*head != NONE)
        g_pages[*head].prev = idx;
    *head = idx;
    g_pages[idx].meta = META_FREE | order;
}

static void unlink_free(size_t idx, unsigned order) {
    struct page *p = &g_pages[idx];
    if (p->prev != NONE)
        g_pages[p->prev].next = p->next;
    else
        g_free[zone_of(idx)][order] = p->next;
    if (p->next != NONE)
        g_pages[p->next].prev = p->prev;
    p->meta = 0;
}

void buddy_init(uintptr_t base, uintptr_t end, struct page *pages) {
    base = PAGE_ALIGN_DOWN(base);
    end = PAGE_ALIGN_UP(end);
    if (end <= base)
        KERNEL_PANIC_HALT("buddy_init: empty region");
    spinlock_init(&g_lock);
    g_pages = pages;
    g_base = base;
    g_count = (end - base) >> PAGE_SHIFT;
    for (size_t i = 0; i < g_count; i++) {
        pages[i].refcount = 0;
        pages[i].meta = META_RESERVED;
    }
    for (unsigned z = 0; z < ZONE_COUNT; z++)
        for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++)
            g_free[z][o] = NONE;
}

void buddy_add_range(uintptr_t start, uintptr_t end) {
    size_t idx = (PAGE_ALIGN_UP(start) - g_base) >> PAGE_SHIFT;
    size_t stop = (PAGE_ALIGN_DOWN(end) - g_base) >> PAGE_SHIFT;

    if (stop > g_count || start < g_base)
        KERNEL_PANIC_HALT("buddy_add_range: outside the managed span");
    for (size_t i = idx; i < stop; i++)
        g_pages[i].meta = 0;
    while (idx < stop) {                /* carve the largest naturally aligned blocks */
        unsigned order = BUDDY_MAX_ORDER;
        while (order > 0 && ((idx & (((size_t)1 << order) - 1)) || idx + ((size_t)1 << order) > stop))
            order--;
        push_free(idx, order);
        g_zone_free[zone_of(idx)] += (size_t)1 << order;
        g_zone_pages[zone_of(idx)] += (size_t)1 << order;
        idx += (size_t)1 << order;
    }
}

/* Take a block of 2^order pages from the given zone; (size_t)-1 if it has none. */
static size_t take(unsigned zone, unsigned order) {
    unsigned o = order;
    while (o <= BUDDY_MAX_ORDER && g_free[zone][o] == NONE)
        o++;
    if (o > BUDDY_MAX_ORDER)
        return (size_t)-1;

    size_t idx = g_free[zone][o];
    unlink_free(idx, o);
    while (o > order) {                 /* split, returning the upper halves */
        o--;
        push_free(idx + ((size_t)1 << o), o);
    }
    g_pages[idx].meta = META_ALLOC | order;
    g_zone_free[zone] -= (size_t)1 << order;
    return idx;
}

static uintptr_t alloc(unsigned order, bool high_ok) {
    if (order > BUDDY_MAX_ORDER)
        return 0;
    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    size_t idx = high_ok ? take(ZONE_HIGH, order) : (size_t)-1;
    if (idx == (size_t)-1)
        idx = take(ZONE_LOW, order);
    spinlock_release_irqrestore(&g_lock, flags);
    return idx == (size_t)-1 ? 0 : g_base + ((uintptr_t)idx << PAGE_SHIFT);
}

uintptr_t buddy_alloc_pages(unsigned order) { return alloc(order, false); }
uintptr_t buddy_alloc_page_user(void) { return alloc(0, true); }

void buddy_free_pages(uintptr_t phys) {
    struct page *pg = buddy_page(phys);
    if (!pg || (phys & (PAGE_SIZE - 1)))
        KERNEL_PANIC_HALT("buddy_free_pages: address not managed");
    size_t idx = (size_t)(pg - g_pages);

    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    if (!(pg->meta & META_ALLOC))
        KERNEL_PANIC_HALT("buddy_free_pages: double free or not a block head");

    unsigned zone = zone_of(idx), order = pg->meta & META_ORDER;
    pg->meta = 0;
    g_zone_free[zone] += (size_t)1 << order;

    while (order < BUDDY_MAX_ORDER) {   /* merge with the buddy while it is free */
        size_t buddy = idx ^ ((size_t)1 << order);
        if (buddy >= g_count || zone_of(buddy) != zone || g_pages[buddy].meta != (META_FREE | order))
            break;
        unlink_free(buddy, order);
        if (buddy < idx)
            idx = buddy;
        order++;
    }
    push_free(idx, order);
    spinlock_release_irqrestore(&g_lock, flags);
}

struct page *buddy_page(uintptr_t phys) {
    if (phys < g_base || phys - g_base >= ((uintptr_t)g_count << PAGE_SHIFT))
        return NULL;
    struct page *pg = &g_pages[(phys - g_base) >> PAGE_SHIFT];
    return (pg->meta & META_RESERVED) ? NULL : pg;
}

size_t buddy_total_pages(unsigned zone) { return g_zone_pages[zone]; }
size_t buddy_free_pages_count(unsigned zone) { return g_zone_free[zone]; }
