/**
 * @file frame.c
 * @brief Reference counts on top of the buddy allocator's single pages.
 */

#include <kernel/memory/frame.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/kmap.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>

static spinlock_t g_ref_lock;

static uintptr_t with_ref(uintptr_t phys) {
    if (phys)
        buddy_page(phys)->refcount = 1;     /* nobody else can see a fresh frame */
    return phys;
}

uintptr_t frame_alloc(void) { return with_ref(buddy_alloc_pages(0)); }
uintptr_t frame_alloc_user(void) { return with_ref(buddy_alloc_page_user()); }

uintptr_t frame_alloc_zeroed(void) {
    uintptr_t phys = frame_alloc();
    if (phys)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

uintptr_t frame_alloc_user_zeroed(void) {
    uintptr_t phys = frame_alloc_user();
    if (phys) {
        void *v = kmap(phys);
        memset(v, 0, PAGE_SIZE);
        kunmap(v);
    }
    return phys;
}

void frame_copy(uintptr_t dst, uintptr_t src) {
    void *d = kmap(dst), *s = kmap(src);
    memcpy(d, s, PAGE_SIZE);
    kunmap(s);
    kunmap(d);
}

void get_frame(uintptr_t phys) {
    struct page *pg = buddy_page(phys & PAGING_ADDR_MASK);
    if (!pg)
        return;
    uintptr_t flags = spinlock_acquire_irqsave(&g_ref_lock);
    if (pg->refcount == 0 || pg->refcount == 0xFFFFu)
        KERNEL_PANIC_HALT("get_frame: bad reference count");
    pg->refcount++;
    spinlock_release_irqrestore(&g_ref_lock, flags);
}

void put_frame(uintptr_t phys) {
    phys &= PAGING_ADDR_MASK;
    struct page *pg = buddy_page(phys);
    if (!pg)
        return;
    uintptr_t flags = spinlock_acquire_irqsave(&g_ref_lock);
    if (pg->refcount == 0)
        KERNEL_PANIC_HALT("put_frame: frame is not allocated");
    uint16_t left = --pg->refcount;
    spinlock_release_irqrestore(&g_ref_lock, flags);
    if (left == 0)
        buddy_free_pages(phys);
}

int get_frame_refcount(uintptr_t phys) {
    struct page *pg = buddy_page(phys & PAGING_ADDR_MASK);
    return pg ? pg->refcount : -1;
}
