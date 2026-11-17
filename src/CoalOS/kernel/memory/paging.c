/**
 * @file paging.c
 * @brief Page-table walker, mapping operations and per-process directories.
 *
 * All directories and tables are frames from the allocator and are reached
 * through the kernel direct map, so there is exactly one walker: paging_pte().
 */

#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/lib/string.h>

bool g_nx_supported = false;
uint32_t *g_kernel_page_directory_virt;
uint32_t g_kernel_page_directory_phys;

static inline uint32_t *table_at(uint32_t entry) {
    return phys_to_virt(entry & PAGING_ADDR_MASK);
}

void paging_flush_tlb(void) {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
}

uint32_t *paging_pte(uint32_t *pd, uintptr_t vaddr, bool create) {
    uint32_t *pde = (uint32_t *)phys_to_virt((uintptr_t)pd) + PDE_INDEX(vaddr);

    if (!(*pde & PAGE_PRESENT)) {
        if (!create)
            return NULL;
        uintptr_t pt = frame_alloc_zeroed();
        if (!pt)
            return NULL;
        /* The PDE is published only once the table is zeroed. */
        *pde = pt | PAGE_PRESENT | PAGE_RW | (vaddr < KERNEL_SPACE_VIRT_START ? PAGE_USER : 0);
    }
    return table_at(*pde) + PTE_INDEX(vaddr);
}

int paging_map_single_4k(uint32_t *pd, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
    if (((vaddr | paddr) & PAGING_OFFSET_MASK) || vaddr < IDENTITY_MAP_SIZE)
        return -1;

    uint32_t *pte = paging_pte(pd, vaddr, true);
    if (!pte)
        return -1;

    uint32_t old = *pte;
    *pte = paddr | (flags & PAGING_FLAG_MASK) | PAGE_PRESENT;
    if (old & PAGE_PRESENT) {
        paging_invalidate_page((void *)vaddr);
        put_frame(old);
    }
    return 0;
}

static bool table_is_empty(const uint32_t *pt) {
    for (unsigned i = 0; i < 1024; i++)
        if (pt[i] & PAGE_PRESENT)
            return false;
    return true;
}

int paging_unmap_range(uint32_t *pd, uintptr_t vaddr, size_t size) {
    if (vaddr & PAGING_OFFSET_MASK)
        return -1;
    uintptr_t end = vaddr + PAGE_ALIGN_UP(size);
    if (end < vaddr && end != 0)
        return -1;                      /* wrapped past the top of the address space */

    uint32_t *pd_virt = phys_to_virt((uintptr_t)pd);
    for (uintptr_t a = vaddr; a != end;) {
        uint32_t *pde = &pd_virt[PDE_INDEX(a)];
        uintptr_t chunk_end = (a | 0x3FFFFFu) + 1;       /* end of this 4 MiB span */
        if (chunk_end == 0 || (end != 0 && chunk_end > end))
            chunk_end = end;

        if (*pde & PAGE_PRESENT) {
            uint32_t *pt = table_at(*pde);
            for (; a != chunk_end; a += PAGE_SIZE) {
                uint32_t *pte = &pt[PTE_INDEX(a)];
                if (*pte & PAGE_PRESENT) {
                    uint32_t old = *pte;
                    *pte = 0;
                    paging_invalidate_page((void *)a);
                    put_frame(old);
                }
            }
            /* Release a user table that no longer maps anything. The shared
             * kernel and identity tables are never freed. */
            uintptr_t span = (a - 1) & ~0x3FFFFFu;
            if (span >= IDENTITY_MAP_SIZE && span < KERNEL_SPACE_VIRT_START && table_is_empty(pt)) {
                *pde = 0;
                put_frame((uintptr_t)pt - KERNEL_SPACE_VIRT_START);
            }
        }
        a = chunk_end;
    }
    return 0;
}

int paging_get_physical_address_and_flags(uint32_t *pd, uintptr_t vaddr,
                                          uintptr_t *paddr_out, uint32_t *flags_out) {
    uint32_t *pte = paging_pte(pd, vaddr, false);
    if (!pte || !(*pte & PAGE_PRESENT))
        return -1;
    *paddr_out = (*pte & PAGING_ADDR_MASK) | (vaddr & PAGING_OFFSET_MASK);
    *flags_out = *pte & PAGING_FLAG_MASK;
    return 0;
}

/* --- Per-process directories ---------------------------------------------- */

void copy_kernel_pde_entries(uint32_t *new_pd_virt) {
    new_pd_virt[0] = g_kernel_page_directory_virt[0];
    for (uint32_t i = KERNEL_PDE_INDEX; i < RECURSIVE_PDE_INDEX; i++)
        new_pd_virt[i] = g_kernel_page_directory_virt[i];
}

uintptr_t paging_new_directory(void) {
    uintptr_t pd = frame_alloc_zeroed();
    if (!pd)
        return 0;
    uint32_t *pd_virt = phys_to_virt(pd);
    copy_kernel_pde_entries(pd_virt);
    pd_virt[RECURSIVE_PDE_INDEX] = pd | PAGE_PRESENT | PAGE_RW;
    return pd;
}

void paging_free_user_space(uint32_t *pd) {
    uint32_t *pd_virt = phys_to_virt((uintptr_t)pd);
    for (uint32_t i = 1; i < KERNEL_PDE_INDEX; i++) {   /* PDE 0 is the shared identity map */
        if (!(pd_virt[i] & PAGE_PRESENT))
            continue;
        uint32_t *pt = table_at(pd_virt[i]);
        for (unsigned j = 0; j < 1024; j++)
            if (pt[j] & PAGE_PRESENT)
                put_frame(pt[j]);
        put_frame(pd_virt[i]);
        pd_virt[i] = 0;
    }
}

uintptr_t paging_clone_directory(uint32_t *src_pd) {
    uint32_t *src = phys_to_virt((uintptr_t)src_pd);
    uintptr_t child = paging_new_directory();
    if (!child)
        return 0;
    uint32_t *dst = phys_to_virt(child);

    for (uint32_t i = 1; i < KERNEL_PDE_INDEX; i++) {
        if (!(src[i] & PAGE_PRESENT))
            continue;
        uintptr_t dpt_phys = frame_alloc_zeroed();
        if (!dpt_phys) {
            paging_free_user_space((uint32_t *)child);
            put_frame(child);
            paging_flush_tlb();
            return 0;
        }
        dst[i] = dpt_phys | (src[i] & PAGING_FLAG_MASK);

        uint32_t *spt = table_at(src[i]);
        uint32_t *dpt = table_at(dst[i]);
        for (unsigned j = 0; j < 1024; j++) {
            uint32_t pte = spt[j];
            if (!(pte & PAGE_PRESENT))
                continue;
            if (pte & PAGE_RW) {                /* both sides copy on first write */
                pte = (pte & ~PAGE_RW) | PAGE_COW;
                spt[j] = pte;
            }
            dpt[j] = pte;
            get_frame(pte);
        }
    }
    paging_flush_tlb();                         /* the parent's PTEs lost write access */
    return child;
}
