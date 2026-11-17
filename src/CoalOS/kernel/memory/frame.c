/**
 * @file frame.c
 * @brief Reference counts on top of the buddy allocator's single pages.
 */

#include <kernel/memory/frame.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>

static uint16_t *g_refcount;       /* one per managed page */
static spinlock_t g_ref_lock;

void frame_init(void) {
    size_t bytes = buddy_total_pages() * sizeof(uint16_t);
    unsigned order = 0;
    while (((size_t)PAGE_SIZE << order) < bytes)
        order++;
    uintptr_t phys = buddy_alloc_pages(order);
    if (!phys)
        KERNEL_PANIC_HALT("frame_init: no memory for reference counts");
    g_refcount = phys_to_virt(phys);
    memset(g_refcount, 0, bytes);
}

uintptr_t frame_alloc(void) {
    uintptr_t phys = buddy_alloc_pages(0);
    if (phys) {
        uintptr_t flags = spinlock_acquire_irqsave(&g_ref_lock);
        g_refcount[buddy_page_index(phys)] = 1;
        spinlock_release_irqrestore(&g_ref_lock, flags);
    }
    return phys;
}

uintptr_t frame_alloc_zeroed(void) {
    uintptr_t phys = frame_alloc();
    if (phys)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

void get_frame(uintptr_t phys) {
    long i = buddy_page_index(phys & PAGING_ADDR_MASK);
    if (i < 0)
        return;
    uintptr_t flags = spinlock_acquire_irqsave(&g_ref_lock);
    if (g_refcount[i] == 0 || g_refcount[i] == 0xFFFFu)
        KERNEL_PANIC_HALT("get_frame: bad reference count");
    g_refcount[i]++;
    spinlock_release_irqrestore(&g_ref_lock, flags);
}

void put_frame(uintptr_t phys) {
    phys &= PAGING_ADDR_MASK;
    long i = buddy_page_index(phys);
    if (i < 0)
        return;
    uintptr_t flags = spinlock_acquire_irqsave(&g_ref_lock);
    if (g_refcount[i] == 0)
        KERNEL_PANIC_HALT("put_frame: frame is not allocated");
    uint16_t left = --g_refcount[i];
    spinlock_release_irqrestore(&g_ref_lock, flags);
    if (left == 0)
        buddy_free_pages(phys);
}

int get_frame_refcount(uintptr_t phys) {
    long i = buddy_page_index(phys & PAGING_ADDR_MASK);
    return i < 0 ? -1 : g_refcount[i];
}

void frame_stats(size_t *total_pages, size_t *free_pages) {
    if (total_pages) *total_pages = buddy_total_pages();
    if (free_pages)  *free_pages = buddy_free_pages_count();
}
