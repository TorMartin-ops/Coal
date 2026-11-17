/**
 * @file dma.c
 * @brief DMA memory and device register mappings (see dma.h).
 */
#include <kernel/memory/dma.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

void *dma_alloc_coherent(size_t size, uintptr_t *phys) {
    void *p = kmalloc(MAX(size, (size_t)2048));   /* above the slab sizes: whole pages, page aligned */
    if (!p) return NULL;
    memset(p, 0, size);
    *phys = dma_phys(p);
    return p;
}

void dma_free_coherent(void *virt) { kfree(virt); }

bool dma_reachable(const void *p, size_t len) {
    uintptr_t a = (uintptr_t)p;
    return a >= PHYS_OFFSET && a < KERNEL_STACK_VADDR_START && len <= KERNEL_STACK_VADDR_START - a;
}
