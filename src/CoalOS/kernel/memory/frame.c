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

/* Zero-filled frames prepared while the CPU is idle, so that a fault or a new
 * page table does not have to clear 4 KiB itself. They still count as free
 * memory and give way to ordinary allocations when the buddy allocator is empty. */
#define ZERO_POOL      32
#define POOL_RESERVE   256              /* pages the buddy allocator keeps before refilling */

static uintptr_t g_pool[ZERO_POOL];
static volatile unsigned g_pool_n;
static size_t g_pool_zone[ZONE_COUNT];
static spinlock_t g_pool_lock;

static inline unsigned zone_of_phys(uintptr_t phys) { return phys >= HIGHMEM_START_PHYS ? ZONE_HIGH : ZONE_NORMAL; }

/* Take a pool frame; only a lowmem one if `low` (page tables must be reachable). */
static uintptr_t pool_pop(bool low) {
    uintptr_t phys = 0;
    if (!g_pool_n)
        return 0;                       /* the common case when busy: no lock */
    uintptr_t flags = spinlock_acquire_irqsave(&g_pool_lock);
    if (g_pool_n && (!low || g_pool[g_pool_n - 1] < HIGHMEM_START_PHYS)) {
        phys = g_pool[--g_pool_n];
        g_pool_zone[zone_of_phys(phys)]--;
    }
    spinlock_release_irqrestore(&g_pool_lock, flags);
    return phys;
}

static void clear_frame(uintptr_t phys) {
    void *v = kmap(phys);
    memset(v, 0, PAGE_SIZE);
    kunmap(v);
}

bool frame_refill_zeroed(void) {
    if (g_pool_n >= ZERO_POOL || buddy_free_pages_count(ZONE_NORMAL) + buddy_free_pages_count(ZONE_HIGH) < POOL_RESERVE)
        return false;
    uintptr_t phys = buddy_alloc_page_user();
    if (!phys)
        return false;
    clear_frame(phys);

    uintptr_t flags = spinlock_acquire_irqsave(&g_pool_lock);
    bool kept = g_pool_n < ZERO_POOL;
    if (kept) {
        g_pool[g_pool_n++] = phys;
        g_pool_zone[zone_of_phys(phys)]++;
    }
    spinlock_release_irqrestore(&g_pool_lock, flags);
    if (!kept)
        buddy_free_pages(phys);
    return true;
}

size_t frame_pool_pages(unsigned zone) { return g_pool_zone[zone]; }

static uintptr_t with_ref(uintptr_t phys) {
    if (phys)
        buddy_page(phys)->refcount = 1;     /* nobody else can see a fresh frame */
    return phys;
}

uintptr_t frame_alloc(void) {
    uintptr_t phys = buddy_alloc_pages(0);
    return with_ref(phys ? phys : pool_pop(true));
}

uintptr_t frame_alloc_user(void) {
    uintptr_t phys = buddy_alloc_page_user();
    return with_ref(phys ? phys : pool_pop(false));
}

uintptr_t frame_alloc_zeroed(void) {
    uintptr_t phys = pool_pop(true);
    if (phys)
        return with_ref(phys);
    phys = frame_alloc();
    if (phys)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

uintptr_t frame_alloc_user_zeroed(void) {
    uintptr_t phys = pool_pop(false);
    if (phys)
        return with_ref(phys);
    phys = frame_alloc_user();
    if (phys)
        clear_frame(phys);
    return phys;
}

void frame_copy(uintptr_t dst, uintptr_t src) {
    void *d = kmap(dst), *s = kmap(src);
    memcpy(d, s, PAGE_SIZE);
    kunmap(s);
    kunmap(d);
}

/* Counts change with one locked xadd: no spinlock, no interrupt toggling. */
static inline uint16_t ref_add(uint16_t *ref, int16_t delta) {
    asm volatile("lock xaddw %0, %1" : "+r"(delta), "+m"(*ref) : : "memory", "cc");
    return (uint16_t)delta;                 /* the value before the addition */
}

void get_frame(uintptr_t phys) {
    struct page *pg = buddy_page(phys & PAGING_ADDR_MASK);
    if (!pg)
        return;
    uint16_t old = ref_add(&pg->refcount, 1);
    if (old == 0 || old == 0xFFFFu)
        KERNEL_PANIC_HALT("get_frame: bad reference count");
}

void put_frame(uintptr_t phys) {
    phys &= PAGING_ADDR_MASK;
    struct page *pg = buddy_page(phys);
    if (!pg)
        return;
    uint16_t old = ref_add(&pg->refcount, -1);
    if (old == 0)
        KERNEL_PANIC_HALT("put_frame: frame is not allocated");
    if (old == 1)
        buddy_free_pages(phys);
}

int get_frame_refcount(uintptr_t phys) {
    struct page *pg = buddy_page(phys & PAGING_ADDR_MASK);
    return pg ? pg->refcount : -1;
}
