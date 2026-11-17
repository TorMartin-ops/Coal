/**
 * @file mm.h
 * @brief Per-process address spaces: VMAs, demand paging, copy-on-write.
 *
 * A VMA describes a page-aligned user range [vm_start, vm_end) with
 * permissions. Pages are mapped lazily: the first touch of an anonymous page
 * allocates a zeroed frame. File-backed VMAs are not implemented (their faults
 * are rejected). VMAs never overlap and never cover the first 4 MiB or kernel
 * space. Functions taking an mm lock mm->lock themselves.
 *
 * Return values: 0 or a positive result on success, negative errno on failure.
 */
#ifndef MM_H
#define MM_H

#include <kernel/core/types.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/rbtree.h>

#define VM_READ         0x00000001
#define VM_WRITE        0x00000002
#define VM_EXEC         0x00000004
#define VM_SHARED       0x00000008
#define VM_GROWS_DOWN   0x00000010  /* stack: grows towards lower addresses on demand */
#define VM_ANONYMOUS    0x00000020
#define VM_FILEBACKED   0x00000040
#define VM_USER         0x00000080
#define VM_HEAP         0x00000100
#define VM_STACK        0x00000200

#define MM_STACK_MAX    (8u * 1024 * 1024)      /* a VM_GROWS_DOWN VMA may grow to this size */
#define MM_BRK_MAX      (256u * 1024 * 1024)
#define MM_MMAP_BASE    0x40000000u             /* anonymous mmap placement window */
#define MM_MMAP_LIMIT   0xB0000000u

typedef struct vma_struct {
    uintptr_t vm_start;         /* inclusive, page aligned */
    uintptr_t vm_end;           /* exclusive, page aligned */
    uint32_t vm_flags;
    uint32_t page_prot;         /* PTE flags, derived from vm_flags */
    struct rb_node rb_node;
    struct mm_struct *vm_mm;
} vma_struct_t;

typedef struct mm_struct {
    struct rb_tree vma_tree;    /* ordered by vm_start */
    uint32_t *pgd_phys;         /* physical address of the page directory */
    spinlock_t lock;            /* protects the VMA tree */
    int map_count;
    uintptr_t start_code, end_code;
    uintptr_t start_data, end_data;
    uintptr_t start_brk, end_brk;   /* program break (see mm_brk()) */
    uintptr_t start_stack;
} mm_struct_t;

/** New empty address space around an existing page directory. */
mm_struct_t *create_mm(uint32_t *pgd_phys);

/** Free all VMAs, user pages and user page tables and the mm itself. The page
 *  directory frame is not freed; its owner releases it with put_frame(). */
void destroy_mm(mm_struct_t *mm);

/** Add a VMA. start and end must be page aligned with start < end and lie in
 *  [4 MiB, KERNEL_SPACE_VIRT_START). NULL on overlap, bad range or OOM. */
vma_struct_t *insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end, uint32_t vm_flags);

/** munmap: unmap and free [start, start+length), splitting or trimming VMAs. */
int remove_vma_range(mm_struct_t *mm, uintptr_t start, size_t length);

/** Bytes of address space covered by the VMAs (for RLIMIT_AS). */
size_t mm_total_vm(mm_struct_t *mm);

/** Move the program break (Linux brk semantics). new_brk == 0 queries.
 *  @return the resulting break; unchanged if the request could not be met. */
uintptr_t mm_brk(mm_struct_t *mm, uintptr_t new_brk);

/** Create an anonymous mapping of len bytes. hint is used if that range is free.
 *  @return the address, or -ENOMEM / -EINVAL. */
intptr_t mm_mmap_anon(mm_struct_t *mm, uintptr_t hint, size_t len, uint32_t vm_flags);

/** Resolve a page fault in a user address space: demand-zero page, stack
 *  growth, copy-on-write.
 *  @return VM_FAULT_OK, VM_FAULT_SEGV or VM_FAULT_OOM (paging_fault.h). */
int mm_fault(mm_struct_t *mm, uintptr_t addr, uint32_t error_code);

/** Check that [addr, addr+len) is covered by VMAs allowing the access. With
 *  populate, also faults every page in (and breaks copy-on-write for writes) so
 *  that the kernel can then touch it without taking a page fault. */
bool mm_access_ok(mm_struct_t *mm, uintptr_t addr, size_t len, bool write, bool populate);

/** Duplicate an address space for fork: new page directory with copy-on-write
 *  pages (paging_clone_directory) and copies of all VMAs and brk/stack fields.
 *  The child's pgd_phys is owned by the caller (release with put_frame() after
 *  destroy_mm()). NULL on failure; the parent is unchanged. */
mm_struct_t *dup_mm(mm_struct_t *parent);

#endif /* MM_H */
