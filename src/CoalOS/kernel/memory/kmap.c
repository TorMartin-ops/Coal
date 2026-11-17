/**
 * @file kmap.c
 * @brief Fixed-slot temporary mappings for highmem frames.
 */

#include <kernel/memory/kmap.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>

static uint32_t g_used;                 /* bit per slot */

static uint32_t *slot_pte(unsigned slot) {
    uint32_t pde = g_kernel_page_directory_virt[PDE_INDEX(KMAP_VADDR_START)];
    return (uint32_t *)phys_to_virt(pde & PAGING_ADDR_MASK) + PTE_INDEX(KMAP_VADDR_START) + slot;
}

void *kmap(uintptr_t phys) {
    if (phys < HIGHMEM_START_PHYS)
        return phys_to_virt(phys);

    uintptr_t flags = local_irq_save();
    unsigned slot = 0;
    while (slot < KMAP_SLOTS && (g_used & (1u << slot)))
        slot++;
    if (slot == KMAP_SLOTS)
        KERNEL_PANIC_HALT("kmap: out of slots");
    g_used |= 1u << slot;
    local_irq_restore(flags);

    void *va = (void *)(KMAP_VADDR_START + slot * PAGE_SIZE);
    *slot_pte(slot) = (phys & PAGING_ADDR_MASK) | PTE_KERNEL_DATA_FLAGS;
    paging_invalidate_page(va);
    return va;
}

void kunmap(void *vaddr) {
    uintptr_t a = (uintptr_t)vaddr;
    if (a < KMAP_VADDR_START || a >= KMAP_VADDR_START + KMAP_SLOTS * PAGE_SIZE)
        return;                         /* a lowmem address */
    unsigned slot = (a - KMAP_VADDR_START) >> PAGE_SHIFT;
    *slot_pte(slot) = 0;
    paging_invalidate_page(vaddr);
    uintptr_t flags = local_irq_save();
    g_used &= ~(1u << slot);
    local_irq_restore(flags);
}
