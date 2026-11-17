/**
 * @file paging_process.h
 * @brief Per-process address spaces (PML4 roots).
 *
 * Fork recipe:
 *   child_mm = dup_mm(parent_mm);   // mm.h: clones the page tables and the VMAs
 * or, at the paging level only:
 *   child_pd = paging_clone_directory(parent_pd);
 */
#ifndef PAGING_PROCESS_H
#define PAGING_PROCESS_H

#include <kernel/core/types.h>

/** Allocate a PML4 containing only the shared kernel entries.
 *  @return physical address of the root, or 0 if out of memory. */
uintptr_t paging_new_directory(void);

/** Duplicate the user address space of src_pd (physical address of its PML4) into a new one.
 *
 *  Every present user page is shared with the child. Writable pages are made
 *  read-only and tagged PAGE_COW in both address spaces; the first write on
 *  either side copies the page (see mm_fault()). Read-only pages stay shared.
 *  The parent's TLB is flushed. On failure the partial child is released and
 *  the parent stays valid.
 *  The caller must also duplicate the VMAs (dup_mm() does both).
 *  @return physical address of the new root, or 0 on failure. */
uintptr_t paging_clone_directory(void *src_pd);

/** Drop every user mapping and user page table of an address space. The PML4 frame
 *  itself is not freed; the owner releases it with put_frame(). */
void paging_free_user_space(void *pd);

#endif /* PAGING_PROCESS_H */
