/**
 * @file syscall_mm.c
 * @brief Memory system calls: brk, anonymous mmap2, munmap.
 */

#include "syscall_internal.h"
#include <kernel/memory/mm.h>
#include <kernel/memory/paging.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/rbtree.h>

#define MMAP_BASE   0x40000000u         /* search for free ranges from here up to the stack */
#define BRK_LIMIT   MMAP_BASE           /* the heap may not run into the mmap area */

static uint32_t vma_prot_flags(uint32_t prot, uint32_t *page_prot) {
    uint32_t vm = VM_ANONYMOUS | VM_USER;

    *page_prot = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_READ)  vm |= VM_READ;
    if (prot & PROT_EXEC)  vm |= VM_READ | VM_EXEC;
    if (prot & PROT_WRITE) { vm |= VM_READ | VM_WRITE; *page_prot |= PAGE_RW; }
    return vm;
}

long sc_brk(uint32_t addr) {
    mm_struct_t *mm = get_current_process()->mm;
    uintptr_t old = mm->end_brk;

    if (addr < mm->start_brk || addr >= BRK_LIMIT)
        return old;                                     /* query or refusal: report the break */

    uintptr_t old_end = PAGE_ALIGN_UP(old);
    uintptr_t new_end = PAGE_ALIGN_UP(addr);
    uint32_t page_prot;
    uint32_t vm = vma_prot_flags(PROT_READ | PROT_WRITE, &page_prot) | VM_HEAP;

    if (new_end > old_end) {
        if (!insert_vma(mm, old_end, new_end, vm, page_prot, NULL, 0))
            return old;
    } else if (new_end < old_end) {
        if (remove_vma_range(mm, new_end, old_end - new_end) != 0)
            return old;
    }
    mm->end_brk = addr;
    return addr;
}

/* First free range of len bytes at or above MMAP_BASE; 0 if none. */
static uintptr_t find_gap(mm_struct_t *mm, size_t len) {
    uintptr_t cand = MMAP_BASE;
    uintptr_t f = spinlock_acquire_irqsave(&mm->lock);

    for (struct rb_node *n = rb_tree_first(&mm->vma_tree); n; n = rb_node_next(n)) {
        vma_struct_t *v = rb_entry(n, vma_struct_t, rb_node);
        if (v->vm_end <= cand) continue;
        if (v->vm_start >= cand + len) break;           /* the gap before v is large enough */
        cand = v->vm_end;
    }
    spinlock_release_irqrestore(&mm->lock, f);
    return cand + len <= USER_STACK_BOTTOM_VIRT ? cand : 0;
}

long sc_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t pgoff) {
    mm_struct_t *mm = get_current_process()->mm;
    uint32_t page_prot;

    (void)pgoff;
    if (!(flags & MAP_ANONYMOUS) || fd != -1) return -ENOSYS;   /* file mappings are not supported */
    if (!len) return -EINVAL;
    if (len > USER_STACK_BOTTOM_VIRT) return -ENOMEM;
    len = PAGE_ALIGN_UP(len);

    uintptr_t at;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || addr < PAGE_SIZE || addr + len > KERNEL_SPACE_VIRT_START)
            return -EINVAL;
        remove_vma_range(mm, addr, len);                /* MAP_FIXED replaces what was there */
        at = addr;
    } else {
        at = find_gap(mm, len);
        if (!at) return -ENOMEM;
    }
    uint32_t vm = vma_prot_flags(prot, &page_prot);
    if (flags & MAP_SHARED) vm |= VM_SHARED;
    return insert_vma(mm, at, at + len, vm, page_prot, NULL, 0) ? (long)at : -ENOMEM;
}

long sc_munmap(uint32_t addr, uint32_t len) {
    mm_struct_t *mm = get_current_process()->mm;

    if ((addr & (PAGE_SIZE - 1)) || !len || addr + len < addr || addr + len > KERNEL_SPACE_VIRT_START)
        return -EINVAL;
    return fs_ret(remove_vma_range(mm, addr, PAGE_ALIGN_UP(len)));
}
