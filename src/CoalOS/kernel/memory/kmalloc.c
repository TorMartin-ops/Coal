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
