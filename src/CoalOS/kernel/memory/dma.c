/**
 * @file dma.c
 * @brief DMA memory and device register mappings (see dma.h).
 */
#include <kernel/memory/dma.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

/* Device registers live between the e1000's window (the first 128 KiB of the MMIO area) and the local APIC page. */
#define IOREMAP_START (KERNEL_MMIO_VADDR_START + 0x100000u)
#define IOREMAP_END   (KERNEL_MMIO_VADDR_START + 0x200000u)

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
    return a >= KERNEL_SPACE_VIRT_START && len <= HIGHMEM_START_PHYS &&
           a - KERNEL_SPACE_VIRT_START <= HIGHMEM_START_PHYS - len;
}

volatile void *ioremap(uint64_t phys, size_t len) {
    static uintptr_t next = IOREMAP_START;
    uintptr_t off = (uintptr_t)(phys & PAGING_OFFSET_MASK);
    size_t pages = (off + len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t *kpd = (uint32_t *)g_kernel_page_directory_phys;

    if (phys > UINTPTR_MAX - len || pages > (IOREMAP_END - next) / PAGE_SIZE) return NULL;
    for (size_t i = 0; i < pages; i++)
        if (paging_map_single_4k(kpd, next + i * PAGE_SIZE, (uintptr_t)phys - off + i * PAGE_SIZE,
                                 PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT) != 0)
            return NULL;
    volatile void *va = (volatile void *)(next + off);
    next += pages * PAGE_SIZE;
    return va;
}
