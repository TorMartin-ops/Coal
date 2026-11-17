/**
 * @file paging_process.h
 * @brief Per-process page directories.
 *
 * Fork recipe:
 *   child_mm = dup_mm(parent_mm);   // mm.h: clones the directory and the VMAs
 * or, at the paging level only:
 *   child_pd = paging_clone_directory(parent_pd);
 */
#ifndef PAGING_PROCESS_H
#define PAGING_PROCESS_H

#include <kernel/core/types.h>

/** Allocate a page directory containing only the shared kernel mappings
 *  (identity map, direct map, kernel stacks, recursive entry).
 *  @return physical address of the directory, or 0 if out of memory. */
uintptr_t paging_new_directory(void);

/** Duplicate the user address space of src_pd_phys into a new directory.
 *
 *  Every present user page is shared with the child. Writable pages are made
 *  read-only and tagged PAGE_COW in both directories; the first write on
 *  either side copies the page (see mm_fault()). Read-only pages stay shared.
 *  The parent's TLB is flushed. On failure the partial child is released and
 *  the parent stays valid.
 *  The caller must also duplicate the VMAs (dup_mm() does both).
 *  @return physical address of the new directory, or 0 on failure. */
uintptr_t paging_clone_directory(uint32_t *src_pd_phys);

/** Drop every user mapping and user page table of a directory. The directory
 *  frame itself is not freed; the owner releases it with put_frame(). */
void paging_free_user_space(uint32_t *page_directory_phys);

/** Copy the shared kernel PDEs (identity map, 0xC0000000 and up, except the
 *  recursive slot) into a directory reachable at new_pd_virt. */
void copy_kernel_pde_entries(uint32_t *new_pd_virt);

#endif /* PAGING_PROCESS_H */
