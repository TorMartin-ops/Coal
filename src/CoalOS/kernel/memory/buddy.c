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
#ifdef COAL_DEBUG_KERNEL
#include <kernel/debug/ksan.h>
#endif

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

#ifdef COAL_DEBUG_KERNEL
/* Debug kernel: a freed block is filled with 0x6b and every page remembers it; the fill is checked when the page
 * is handed out again, so a write through a stale pointer shows up. Pages never freed before carry no fill. */
#define DBG_POISON 0x6b

static void poison_pages(size_t idx, unsigned order) {
    uintptr_t lo = g_base + ((uintptr_t)idx << PAGE_SHIFT), cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (cr3 >= lo && cr3 < lo + (PAGE_SIZE << order))
        ksan_report(KSAN_MEM, "freeing page %#lx, the page directory loaded in CR3", (unsigned long)lo);
    memset(phys_to_virt(g_base + ((uintptr_t)idx << PAGE_SHIFT)), DBG_POISON, PAGE_SIZE << order);
    for (size_t i = 0; i < ((size_t)1 << order); i++)
        g_pages[idx + i].dbg = 1;
}

static void check_pages(size_t idx, unsigned order) {
    for (size_t i = 0; i < ((size_t)1 << order); i++) {
        struct page *pg = &g_pages[idx + i];
        if (!pg->dbg)
            continue;
        pg->dbg = 0;
        const uint8_t *p = phys_to_virt(g_base + ((uintptr_t)(idx + i) << PAGE_SHIFT));
        size_t off = 0;
        while (off < PAGE_SIZE && p[off] == DBG_POISON)
            off++;
        if (off < PAGE_SIZE)
            ksan_report(KSAN_MEM, "freed page %#lx was written after it was freed (first byte at offset %lu, value %#x)",
                        (unsigned long)(g_base + ((uintptr_t)(idx + i) << PAGE_SHIFT)), (unsigned long)off, p[off]);
    }
}

int buddy_order_of(uintptr_t phys) {
    struct page *pg = buddy_page(phys);
    return pg && (pg->meta & META_ALLOC) && !(phys & (PAGE_SIZE - 1)) ? (int)(pg->meta & META_ORDER) : -1;
}
#endif

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
#ifdef COAL_DEBUG_KERNEL
    if (idx != (size_t)-1)
        check_pages(idx, order);
#endif
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
#ifdef COAL_DEBUG_KERNEL
    poison_pages(idx, order);
#endif
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
