/**
 * @file kmalloc.c
 * @brief kmalloc/kfree on top of slab caches and the buddy allocator.
 */

#include <kernel/memory/kmalloc.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/buddy.h>
#include <kernel/memory/paging.h>

#define MIN_CLASS_SHIFT 4                       /* 16 bytes */
#define NUM_CLASSES     7                       /* 16 .. 1024 */
#define MAX_SLAB_SIZE   (16u << (NUM_CLASSES - 1))

static slab_cache_t g_caches[NUM_CLASSES];

#ifdef COAL_DEBUG_KERNEL
#include <kernel/memory/heapdebug.h>
#include <kernel/memory/kfence.h>
#include <kernel/debug/ksan.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#endif

void kmalloc_init(void) {
    for (int c = 0; c < NUM_CLASSES; c++)
        slab_cache_init(&g_caches[c], (size_t)16 << c);
}

/* Smallest class whose size is >= n (1 <= n <= MAX_SLAB_SIZE). */
static inline unsigned class_of(size_t n) {
    if (n <= 16)
        return 0;
    return 32 - __builtin_clz((unsigned)n - 1) - MIN_CLASS_SHIFT;
}

#ifdef COAL_DEBUG_KERNEL
/* Debug kernel: slab objects get red zones and poison (slab.c); blocks of pages carry the same trailer at the end of
 * the block; a sample of small allocations lives in the KFENCE pool. Blocks up to 256 KiB wait in a quarantine. */

#define BIG_QUARANTINE 32
#define BIG_QUARANTINE_MAX (256u << 10)

static void *g_bigq[BIG_QUARANTINE];
static unsigned g_bigq_head, g_bigq_count;
static spinlock_t g_bigq_lock;
static uint8_t g_rz_ok[HD_RZ_LEN];

static inline size_t big_cap(unsigned order) { return ((size_t)PAGE_SIZE << order) - sizeof(struct hd_trailer); }

static void *big_alloc(size_t size, const uintptr_t *bt, unsigned n) {
    size_t need = size + sizeof(struct hd_trailer);
    if (need > ((size_t)PAGE_SIZE << BUDDY_MAX_ORDER))
        return NULL;
    size_t pages = (need + PAGE_SIZE - 1) >> PAGE_SHIFT;
    unsigned order = pages > 1 ? 32 - __builtin_clz((unsigned)pages - 1) : 0;
    uintptr_t phys = buddy_alloc_pages(order);
    if (!phys)
        return NULL;
    void *p = phys_to_virt(phys);
    struct hd_trailer *t = (struct hd_trailer *)((char *)p + big_cap(order));
    memset(t->rz, HD_RZ_BYTE, HD_RZ_LEN);
    memset(t->rz_next, HD_RZ_BYTE, HD_RZ_LEN);
    hd_on_alloc(p, big_cap(order), size, t, bt, n);
    return p;
}

static void big_release(void *p) {
    int order = buddy_order_of(virt_to_phys(p));
    if (order < 0)
        return;
    hd_check_poison(p, big_cap((unsigned)order), (struct hd_trailer *)((char *)p + big_cap((unsigned)order)), "quarantine");
    buddy_free_pages(virt_to_phys(p));
}

static void big_free(void *p, const uintptr_t *bt, unsigned n) {
    int order = buddy_order_of(virt_to_phys(p));
    if (order < 0) {
        hd_bad_free(p, "not the start of an allocated block of pages");
        return;
    }
    size_t cap = big_cap((unsigned)order);
    struct hd_trailer *t = (struct hd_trailer *)((char *)p + cap);
    if (!g_rz_ok[0])
        memset(g_rz_ok, HD_RZ_BYTE, HD_RZ_LEN);
    if (!hd_on_free(p, cap, t, g_rz_ok, bt, n))
        return;
    if (cap > BIG_QUARANTINE_MAX) {
        buddy_free_pages(virt_to_phys(p));
        return;
    }
    void *old = NULL;
    uintptr_t flags = spinlock_acquire_irqsave(&g_bigq_lock);
    if (g_bigq_count == BIG_QUARANTINE) {
        old = g_bigq[g_bigq_head];
        g_bigq_head = (g_bigq_head + 1) % BIG_QUARANTINE;
        g_bigq_count--;
    }
    g_bigq[(g_bigq_head + g_bigq_count++) % BIG_QUARANTINE] = p;
    spinlock_release_irqrestore(&g_bigq_lock, flags);
    if (old)
        big_release(old);
}

void *kmalloc(size_t size) {
    uintptr_t bt[HD_BT];
    unsigned n;

    if (size == 0)
        return NULL;
    n = ksan_capture(bt, HD_BT);
    if (g_kfence_interval && --g_kfence_countdown == 0) {
        void *p = kfence_alloc(size, bt, n);
        if (p)
            return p;
    }
    if (size <= MAX_SLAB_SIZE)
        return slab_alloc_req(&g_caches[class_of(size)], size, bt, n);
    return big_alloc(size, bt, n);
}

void kfree(void *ptr) {
    uintptr_t bt[HD_BT];
    unsigned n;

    if (!ptr)
        return;
    n = ksan_capture(bt, HD_BT);
    if (kfence_owns(ptr))
        kfence_free(ptr, bt, n);
    else if ((uintptr_t)ptr < PHYS_OFFSET || (uintptr_t)ptr >= PHYS_OFFSET + (512ul << 30))
        hd_bad_free(ptr, "not a kernel heap address");
    else if ((uintptr_t)ptr & PAGING_OFFSET_MASK)
        slab_free_bt(ptr, bt, n);
    else
        big_free(ptr, bt, n);
}

#else
void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    if (size <= MAX_SLAB_SIZE)
        return slab_alloc(&g_caches[class_of(size)]);

    if (size > ((size_t)PAGE_SIZE << BUDDY_MAX_ORDER))
        return NULL;
    size_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    unsigned order = pages > 1 ? 32 - __builtin_clz((unsigned)pages - 1) : 0;
    uintptr_t phys = buddy_alloc_pages(order);
    return phys ? phys_to_virt(phys) : NULL;
}

void kfree(void *ptr) {
    if (!ptr)
        return;
    if ((uintptr_t)ptr & PAGING_OFFSET_MASK)
        slab_free(ptr);                 /* slab objects start after the slab header */
    else
        buddy_free_pages(virt_to_phys(ptr));
}
#endif
