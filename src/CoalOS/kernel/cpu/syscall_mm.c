/**
 * @file syscall_mm.c
 * @brief Memory system calls: brk, anonymous mmap, munmap, mprotect, madvise.
 */

#include "syscall_internal.h"
#include <kernel/memory/mm.h>
#include <kernel/memory/paging.h>

#define MAP_GROWSDOWN   0x0100
#define MAP_NORESERVE   0x4000
#define MAP_POPULATE    0x8000
#define MAP_STACK       0x20000

static uint32_t vm_rights(uint32_t prot) {
    return ((prot & PROT_READ) ? VM_READ : 0) | ((prot & PROT_WRITE) ? VM_READ | VM_WRITE : 0) |
           ((prot & PROT_EXEC) ? VM_READ | VM_EXEC : 0);
}

static bool user_range(uintptr_t addr, size_t len) {
    return !(addr & (PAGE_SIZE - 1)) && len && addr >= USER_SPACE_MIN && addr + len > addr && addr + len <= USER_SPACE_MAX;
}

long sc_brk(uintptr_t addr) {
    pcb_t *p = get_current_process();
    uintptr_t top = p->mm->end_brk;

    if (addr > top && !rlimit_vm_allowed(p, PAGE_ALIGN_UP(addr) - PAGE_ALIGN_UP(top)))
        return top;                                 /* brk reports failure by returning the old break */
    return mm_brk(p->mm, addr);
}

/* Anonymous memory, or for a private file mapping a copy of the file range taken now. */
long sc_mmap(uintptr_t addr, size_t len, uint32_t prot, uint32_t flags, int fd, long off) {
    bool file = !(flags & MAP_ANONYMOUS);
    uint32_t rights = vm_rights(prot);
    uint32_t vm = VM_ANONYMOUS | VM_USER | (file ? (VM_READ | VM_WRITE) : rights);

    if (file && (flags & MAP_SHARED)) return -ENOSYS;           /* shared file mappings are not supported */
    if (!len) return -EINVAL;
    if (file && (off < 0 || (off & (PAGE_SIZE - 1)))) return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (flags & MAP_SHARED) vm |= VM_SHARED;

    if (!rlimit_vm_allowed(get_current_process(), len)) return -ENOMEM;
    mm_struct_t *mm = get_current_process()->mm;
    long at;
    if (flags & MAP_FIXED) {                        /* replaces what was mapped there */
        if (!user_range(addr, len)) return -EINVAL;
        long rc = remove_vma_range(mm, addr, len);
        if (rc < 0) return rc;
        at = insert_vma(mm, addr, addr + len, vm) ? (long)addr : -ENOMEM;
    } else {
        at = mm_mmap_anon(mm, addr, len, vm);
    }
    if (at < 0 || !file) return at;

    long n = sc_pread64((uint32_t)fd, (uintptr_t)at, len, off);
    long rc = n < 0 ? n : (rights == (VM_READ | VM_WRITE) ? 0 : mm_protect(mm, (uintptr_t)at, len, rights));
    if (rc < 0) { remove_vma_range(mm, (uintptr_t)at, len); return rc == -ESPIPE ? -ENODEV : rc; }
    return at;
}

long sc_munmap(uintptr_t addr, size_t len) {
    if (!user_range(addr, PAGE_ALIGN_UP(len))) return -EINVAL;
    return remove_vma_range(get_current_process()->mm, addr, PAGE_ALIGN_UP(len));
}

long sc_mprotect(uintptr_t addr, size_t len, uint32_t prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    return mm_protect(get_current_process()->mm, addr, len, vm_rights(prot));
}

/* Advice never changes behaviour here. */
long sc_madvise(uintptr_t addr, size_t len, int advice) {
    (void)len; (void)advice;
    return addr & (PAGE_SIZE - 1) ? -EINVAL : 0;
}
