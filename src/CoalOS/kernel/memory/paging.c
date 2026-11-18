/**
 * @file paging.c
 * @brief Page-table walker, mapping operations and per-process address spaces.
 *
 * All tables are frames from the allocator and are reached through the kernel direct map, so there is
 * exactly one walker: paging_pte(). Only 4 KiB leaf mappings are created after boot; the 2 MiB pages of
 * the direct map are never split.
 */

#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/cpu/msr.h>
#include <kernel/lib/string.h>

void *g_kernel_page_directory_virt;
uintptr_t g_kernel_page_directory_phys;

static const uintptr_t LEVEL_SHIFT[4] = { 12, 21, 30, 39 };     /* PT, PD, PDPT, PML4 */

static inline pte_t *table_at(pte_t entry) {
    return phys_to_virt(entry & PAGING_ADDR_MASK);
}

static inline unsigned index_at(uintptr_t vaddr, unsigned level) {
    return (vaddr >> LEVEL_SHIFT[level]) & 0x1FF;
}

static inline pte_t *root_of(void *pd) {
    return phys_to_virt((uintptr_t)pd);
}

void paging_flush_tlb(void) {
    write_cr3(read_cr3());
}

/* The table that `entry` points to, allocating a zeroed one if absent and `create` is set. */
static pte_t *next_table(pte_t *entry, uintptr_t vaddr, bool create) {
    if (!(*entry & PAGE_PRESENT)) {
        if (!create)
            return NULL;
        uintptr_t table = frame_alloc_zeroed();
        if (!table)
            return NULL;
        /* Published only once the table is zeroed. */
        *entry = table | PAGE_PRESENT | PAGE_RW | (vaddr < USER_SPACE_MAX ? PAGE_USER : 0);
    }
    return (*entry & PAGE_HUGE) ? NULL : table_at(*entry);
}

pte_t *paging_pte(void *pd, uintptr_t vaddr, bool create) {
    pte_t *table = root_of(pd);

    for (unsigned level = 3; level > 0; level--) {
        table = next_table(&table[index_at(vaddr, level)], vaddr, create);
        if (!table)
            return NULL;
    }
    return &table[index_at(vaddr, 0)];
}

int paging_map_single_4k(void *pd, uintptr_t vaddr, uintptr_t paddr, uint64_t flags) {
    if (((vaddr | paddr) & PAGING_OFFSET_MASK) || (vaddr < USER_SPACE_MIN))
        return -1;

    pte_t *pte = paging_pte(pd, vaddr, true);
    if (!pte)
        return -1;

    pte_t old = *pte;
    *pte = paddr | (flags & PAGING_FLAG_MASK) | PAGE_PRESENT;
    if (old & PAGE_PRESENT) {
        paging_invalidate_page((void *)vaddr);
        put_frame(old);
    }
    return 0;
}

static bool table_is_empty(const pte_t *pt) {
    for (unsigned i = 0; i < PTES_PER_TABLE; i++)
        if (pt[i] & PAGE_PRESENT)
            return false;
    return true;
}

/* The page-directory entry that maps vaddr's 2 MiB span, or NULL if a table above it is absent. */
static pte_t *pde_for(void *pd, uintptr_t vaddr) {
    pte_t *table = root_of(pd);

    for (unsigned level = 3; level > 1; level--) {
        pte_t e = table[index_at(vaddr, level)];
        if (!(e & PAGE_PRESENT) || (e & PAGE_HUGE))
            return NULL;
        table = table_at(e);
    }
    return &table[index_at(vaddr, 1)];
}

int paging_unmap_range(void *pd, uintptr_t vaddr, size_t size) {
    if (vaddr & PAGING_OFFSET_MASK)
        return -1;
    uintptr_t end = vaddr + PAGE_ALIGN_UP(size);
    if (end < vaddr)
        return -1;                      /* wrapped past the top of the address space */

    for (uintptr_t a = vaddr; a < end;) {
        uintptr_t chunk_end = (a | 0x1FFFFFul) + 1;      /* end of this 2 MiB span */
        if (chunk_end > end || chunk_end < a)
            chunk_end = end;

        pte_t *pde = pde_for(pd, a);
        if (pde && (*pde & PAGE_PRESENT) && !(*pde & PAGE_HUGE)) {
            pte_t *pt = table_at(*pde);
            for (; a != chunk_end; a += PAGE_SIZE) {
                pte_t *pte = &pt[PT_INDEX(a)];
                if (*pte & PAGE_PRESENT) {
                    pte_t old = *pte;
                    *pte = 0;
                    paging_invalidate_page((void *)a);
                    put_frame(old);
                }
            }
            /* Release a user table that no longer maps anything; kernel tables are shared and stay. */
            if (chunk_end <= USER_SPACE_MAX && table_is_empty(pt)) {
                put_frame(*pde);
                *pde = 0;
            }
        }
        a = chunk_end;
    }
    return 0;
}

int paging_get_physical_address_and_flags(void *pd, uintptr_t vaddr, uintptr_t *paddr_out, uint64_t *flags_out) {
    pte_t *pte = paging_pte(pd, vaddr, false);
    if (!pte || !(*pte & PAGE_PRESENT))
        return -1;
    *paddr_out = (*pte & PAGING_ADDR_MASK) | (vaddr & PAGING_OFFSET_MASK);
    *flags_out = *pte & PAGING_FLAG_MASK;
    return 0;
}

/* --- Device registers ------------------------------------------------------- */

volatile void *ioremap(uint64_t phys, size_t size) {
    static uintptr_t next = KERNEL_MMIO_VADDR_START + IOREMAP_OFFSET;
    uintptr_t first = PAGE_ALIGN_DOWN(phys), last = PAGE_ALIGN_UP(phys + size);
    uintptr_t va = next;

    if (va + (last - first) > KERNEL_MMIO_VADDR_END)
        return NULL;
    for (uintptr_t off = 0; off < last - first; off += PAGE_SIZE)
        if (paging_map_single_4k((void *)g_kernel_page_directory_phys, va + off, first + off,
                                 PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT | PAGE_NX | PAGE_GLOBAL) != 0)
            return NULL;
    next += last - first;
    return (volatile void *)(va + (phys - first));
}

/* --- Per-process address spaces -------------------------------------------- */

uintptr_t paging_new_directory(void) {
    uintptr_t pd = frame_alloc_zeroed();
    if (!pd)
        return 0;
    pte_t *root = phys_to_virt(pd), *kernel = g_kernel_page_directory_virt;
    for (unsigned i = KERNEL_PML4_FIRST; i < PTES_PER_TABLE; i++)
        root[i] = kernel[i];
    return pd;
}

/* Unused entries are always zero, so a table is mostly skipped eight entries at a time. */
static inline bool empty8(const pte_t *t) {
    return !(t[0] | t[1] | t[2] | t[3] | t[4] | t[5] | t[6] | t[7]);
}

/* Drops every frame below `table` (a table at `level`, 0 = page table) and the tables themselves. */
static void free_table(pte_t *table, unsigned level) {
    for (unsigned i = 0; i < PTES_PER_TABLE; i++) {
        if (!(i & 7) && empty8(&table[i])) {
            i += 7;
            continue;
        }
        pte_t e = table[i];
        if (!(e & PAGE_PRESENT))
            continue;
        if (level)
            free_table(table_at(e), level - 1);
        put_frame(e);                           /* the leaf frame, or the table below */
        table[i] = 0;
    }
}

void paging_free_user_space(void *pd) {
    pte_t *root = root_of(pd);

    for (unsigned i = 0; i < KERNEL_PML4_FIRST; i++) {
        if (!(root[i] & PAGE_PRESENT))
            continue;
        free_table(table_at(root[i]), 2);
        put_frame(root[i]);
        root[i] = 0;
    }
}

/* Copies the table `src` at `level` into `dst`; leaf frames are shared and made copy-on-write (PAGE_SHARED ones stay writable). */
static bool clone_table(pte_t *dst, pte_t *src, unsigned level) {
    for (unsigned i = 0; i < PTES_PER_TABLE; i++) {
        if (!(i & 7) && empty8(&src[i])) {     /* dst is a zeroed frame: nothing to copy */
            i += 7;
            continue;
        }
        pte_t e = src[i];
        if (!(e & PAGE_PRESENT))
            continue;
        if (level == 0) {
            if ((e & PAGE_RW) && !(e & PAGE_SHARED)) {   /* both sides copy on first write */
                e = (e & ~PAGE_RW) | PAGE_COW;
                src[i] = e;
            }
            get_frame(e);
            dst[i] = e;
            continue;
        }
        uintptr_t child = frame_alloc_zeroed();
        if (!child)
            return false;
        dst[i] = child | (e & PAGING_FLAG_MASK);   /* owned now: freed with the rest if cloning fails below */
        if (!clone_table(phys_to_virt(child), table_at(e), level - 1))
            return false;
    }
    return true;
}

uintptr_t paging_clone_directory(void *src_pd) {
    uintptr_t child = paging_new_directory();
    if (!child)
        return 0;
    pte_t *src = root_of(src_pd), *dst = phys_to_virt(child);

    for (unsigned i = 0; i < KERNEL_PML4_FIRST; i++) {
        if (!(src[i] & PAGE_PRESENT))
            continue;
        uintptr_t pdpt = frame_alloc_zeroed();
        if (!pdpt) {
            paging_free_user_space((void *)child);
            put_frame(child);
            paging_flush_tlb();
            return 0;
        }
        dst[i] = pdpt | (src[i] & PAGING_FLAG_MASK);
        if (!clone_table(phys_to_virt(pdpt), table_at(src[i]), 2)) {
            paging_free_user_space((void *)child);
            put_frame(child);
            paging_flush_tlb();
            return 0;
        }
    }
    paging_flush_tlb();                         /* the parent's PTEs lost write access */
    return child;
}
