/**
 * @file syscall_mm.c
 * @brief Memory system calls: brk, anonymous mmap2, munmap.
 */

#include "syscall_internal.h"
#include <kernel/memory/mm.h>
#include <kernel/memory/paging.h>

long sc_brk(uint32_t addr) {
    pcb_t *p = get_current_process();
    uintptr_t top = p->mm->end_brk;

    if (addr > top && !rlimit_vm_allowed(p, PAGE_ALIGN_UP(addr) - PAGE_ALIGN_UP(top)))
        return top;                                 /* brk reports failure by returning the old break */
    return mm_brk(p->mm, addr);
}

long sc_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t pgoff) {
    uint32_t vm = VM_ANONYMOUS | VM_USER;

    (void)pgoff;
    if (!(flags & MAP_ANONYMOUS) || fd != -1) return -ENOSYS;   /* file mappings are not supported */
    if (!len) return -EINVAL;
    if (prot & PROT_READ)  vm |= VM_READ;
    if (prot & PROT_EXEC)  vm |= VM_READ | VM_EXEC;
    if (prot & PROT_WRITE) vm |= VM_READ | VM_WRITE;
    if (flags & MAP_SHARED) vm |= VM_SHARED;

    if (!rlimit_vm_allowed(get_current_process(), PAGE_ALIGN_UP(len))) return -ENOMEM;
    mm_struct_t *mm = get_current_process()->mm;
    if (flags & MAP_FIXED) {                        /* replaces what was mapped there */
        if ((addr & (PAGE_SIZE - 1)) || addr + len < addr || addr + len > KERNEL_SPACE_VIRT_START)
            return -EINVAL;
        if (addr < USER_SPACE_MIN) return -EINVAL;
        long rc = remove_vma_range(mm, addr, PAGE_ALIGN_UP(len));
        if (rc < 0) return rc;
        return insert_vma(mm, addr, addr + PAGE_ALIGN_UP(len), vm | VM_USER | VM_ANONYMOUS) ? (long)addr : -ENOMEM;
    }
    return mm_mmap_anon(mm, addr, PAGE_ALIGN_UP(len), vm);
}

long sc_munmap(uint32_t addr, uint32_t len) {
    if ((addr & (PAGE_SIZE - 1)) || !len || addr + len < addr || addr + len > KERNEL_SPACE_VIRT_START)
        return -EINVAL;
    return remove_vma_range(get_current_process()->mm, addr, PAGE_ALIGN_UP(len));
}
