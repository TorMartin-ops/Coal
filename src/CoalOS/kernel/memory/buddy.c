/**
 * @file buddy.c
 * @brief Binary buddy page allocator.
 *
 * Free blocks are kept on doubly linked lists threaded through the free pages
 * themselves (via the direct map), so removing a buddy while merging is O(1).
 * A byte of metadata per page tells whether it heads a free block or an
 * allocated block and of which order; interior pages carry 0.
 */

#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>

#define META_FREE  0x80u
#define META_ALLOC 0x40u
#define META_ORDER 0x1Fu

struct free_node {
    struct free_node *next, *prev;
};

static struct free_node *g_free[BUDDY_MAX_ORDER + 1];
static uint8_t *g_meta;             /* one byte per managed page, kept at the start of the region */
static uintptr_t g_base;
static size_t g_pages;
static size_t g_free_pages;
static spinlock_t g_lock;

static inline struct free_node *node_at(size_t idx) {
    return phys_to_virt(g_base + ((uintptr_t)idx << PAGE_SHIFT));
}

static inline size_t index_of(const struct free_node *n) {
    return (virt_to_phys(n) - g_base) >> PAGE_SHIFT;
}

static void push_free(size_t idx, unsigned order) {
    struct free_node *n = node_at(idx);
    n->prev = NULL;
    n->next = g_free[order];
    if (n->next)
        n->next->prev = n;
    g_free[order] = n;
    g_meta[idx] = META_FREE | order;
}

static void unlink_free(size_t idx, unsigned order) {
    struct free_node *n = node_at(idx);
    if (n->prev)
        n->prev->next = n->next;
    else
        g_free[order] = n->next;
    if (n->next)
        n->next->prev = n->prev;
    g_meta[idx] = 0;
}

void buddy_init(uintptr_t phys_start, size_t size) {
    uintptr_t start = PAGE_ALIGN_UP(phys_start);
    if (size <= start - phys_start)
        KERNEL_PANIC_HALT("buddy_init: empty region");
    size_t total = (size - (start - phys_start)) >> PAGE_SHIFT;
    if (total > BUDDY_MAX_PAGES)
        total = BUDDY_MAX_PAGES;

    /* The metadata array lives in the first pages of the region itself. */
    size_t meta_pages = (total + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (total <= meta_pages)
        KERNEL_PANIC_HALT("buddy_init: region too small");
    g_meta = phys_to_virt(start);
    memset(g_meta, 0, total - meta_pages);

    spinlock_init(&g_lock);
    g_base = start + ((uintptr_t)meta_pages << PAGE_SHIFT);
    size_t pages = total - meta_pages;
    g_pages = g_free_pages = pages;

    /* Carve the region into the largest naturally aligned blocks. */
    size_t idx = 0;
    while (idx < pages) {
        unsigned order = BUDDY_MAX_ORDER;
        while (order > 0 && ((idx & ((1u << order) - 1)) || idx + (1u << order) > pages))
            order--;
        push_free(idx, order);
        idx += (size_t)1 << order;
    }
}

uintptr_t buddy_alloc_pages(unsigned order) {
    if (order > BUDDY_MAX_ORDER)
        return 0;

    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    unsigned o = order;
    while (o <= BUDDY_MAX_ORDER && !g_free[o])
        o++;
    if (o > BUDDY_MAX_ORDER) {
        spinlock_release_irqrestore(&g_lock, flags);
        return 0;
    }

    size_t idx = index_of(g_free[o]);
    unlink_free(idx, o);
    while (o > order) {             /* split, returning the upper halves */
        o--;
        push_free(idx + ((size_t)1 << o), o);
    }
    g_meta[idx] = META_ALLOC | order;
    g_free_pages -= (size_t)1 << order;
    spinlock_release_irqrestore(&g_lock, flags);

    return g_base + ((uintptr_t)idx << PAGE_SHIFT);
}

void buddy_free_pages(uintptr_t phys) {
    long l = buddy_page_index(phys);
    if (l < 0 || (phys & (PAGE_SIZE - 1)))
        KERNEL_PANIC_HALT("buddy_free_pages: address not managed");
    size_t idx = (size_t)l;

    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    if (!(g_meta[idx] & META_ALLOC))
        KERNEL_PANIC_HALT("buddy_free_pages: double free or not a block head");

    unsigned order = g_meta[idx] & META_ORDER;
    g_meta[idx] = 0;
    g_free_pages += (size_t)1 << order;

    while (order < BUDDY_MAX_ORDER) {   /* merge with the buddy while it is free */
        size_t buddy = idx ^ ((size_t)1 << order);
        if (buddy >= g_pages || g_meta[buddy] != (META_FREE | order))
            break;
        unlink_free(buddy, order);
        if (buddy < idx)
            idx = buddy;
        order++;
    }
    push_free(idx, order);
    spinlock_release_irqrestore(&g_lock, flags);
}

long buddy_page_index(uintptr_t phys) {
    if (phys < g_base || phys >= g_base + ((uintptr_t)g_pages << PAGE_SHIFT))
        return -1;
    return (long)((phys - g_base) >> PAGE_SHIFT);
}

size_t buddy_total_pages(void) { return g_pages; }
size_t buddy_free_pages_count(void) { return g_free_pages; }
