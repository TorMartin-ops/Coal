#include <kernel/process/elf_loader.h>
#include <kernel/process/elf.h>
#include <kernel/process/process.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/paging.h>
#include <kernel/lib/string.h>
#include <kernel/fs/vfs/fs_errno.h>

#define MAX_PHNUM       64
#define MAX_IMAGE_BYTES (128u << 20)     /* sanity cap on the sum of all p_memsz */

static uint32_t seg_page_flags(uint32_t pf) {
    uint32_t f = PAGE_PRESENT | PAGE_USER;
    if (pf & PF_W) f |= PAGE_RW;
    if (!(pf & PF_X) && g_nx_supported) f |= PAGE_NX_BIT;
    return f;
}

/* Maps one page of a segment and copies the file bytes that land in it. */
static int load_page(uint32_t *pd, uintptr_t page, bool shared, uint32_t flags,
                     const uint8_t *src, size_t n, size_t dst_off) {
    uintptr_t phys = 0;
    uint32_t old_flags = 0;
    bool fresh = !shared;

    if (fresh) {
        phys = frame_alloc();
        if (!phys) return -ENOMEM;
    } else if (paging_get_physical_address_and_flags(pd, page, &phys, &old_flags) != 0) {
        return -ENOEXEC;
    }

    uint8_t *v = paging_temp_map(phys);
    if (!v) {
        if (fresh) put_frame(phys);
        return -ENOMEM;
    }
    if (fresh) memset(v, 0, PAGE_SIZE);
    if (n) memcpy(v + dst_off, src, n);
    paging_temp_unmap(v);

    if (!fresh) {                            /* page shared with the previous segment: union of rights */
        flags |= old_flags & PAGE_RW;
        if (!(old_flags & PAGE_NX_BIT)) flags &= ~PAGE_NX_BIT;
    }
    if (!fresh) get_frame(phys);             /* remapping releases the old reference */
    if (paging_map_single_4k(pd, page, phys, flags) != 0) {
        if (!fresh) put_frame(phys);
        if (fresh) put_frame(phys);
        return -ENOMEM;
    }
    return 0;
}

int elf_load(mm_struct_t *mm, const void *file, size_t size, elf_info_t *info) {
    const uint8_t *base = file;
    const Elf32_Ehdr *eh = file;
    const Elf32_Phdr *ph;
    uint32_t prev_end = 0, mapped_end = 0, high = 0, total = 0;
    bool entry_ok = false;

    if (size < sizeof(*eh) ||
        eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_version != EV_CURRENT || eh->e_type != ET_EXEC || eh->e_machine != EM_386 ||
        eh->e_phentsize != sizeof(Elf32_Phdr) || eh->e_phnum == 0 || eh->e_phnum > MAX_PHNUM)
        return -ENOEXEC;
    if (eh->e_phoff > size || (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > size - eh->e_phoff)
        return -ENOEXEC;
    if (eh->e_entry < USER_SPACE_MIN || eh->e_entry >= USER_STACK_BOTTOM_VIRT)
        return -ENOEXEC;

    ph = (const Elf32_Phdr *)(base + eh->e_phoff);
    memset(info, 0, sizeof(*info));
    info->entry = eh->e_entry;
    info->phent = sizeof(Elf32_Phdr);
    info->phnum = eh->e_phnum;

    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *p = &ph[i];
        uint32_t end;

        if (p->p_type == PT_INTERP) return -ENOEXEC;       /* no dynamic linking */
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;

        end = p->p_vaddr + p->p_memsz;
        if (p->p_filesz > p->p_memsz || end < p->p_vaddr ||
            p->p_vaddr < USER_SPACE_MIN || end > USER_STACK_BOTTOM_VIRT ||
            p->p_vaddr < prev_end ||                        /* sorted, non-overlapping */
            p->p_offset > size || p->p_filesz > size - p->p_offset)
            return -ENOEXEC;
        total += p->p_memsz;
        if (total > MAX_IMAGE_BYTES) return -ENOEXEC;
        prev_end = end;
        if (end > high) high = end;
        if ((p->p_flags & PF_X) && eh->e_entry >= p->p_vaddr && eh->e_entry < end) entry_ok = true;
        if (!info->phdr && eh->e_phoff >= p->p_offset &&
            eh->e_phoff - p->p_offset < p->p_filesz)
            info->phdr = p->p_vaddr + (eh->e_phoff - p->p_offset);
    }
    if (!prev_end || !entry_ok) return -ENOEXEC;
    info->brk = (uint32_t)PAGE_ALIGN_UP(high);

    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;

        uint32_t flags = seg_page_flags(p->p_flags);
        uintptr_t first = PAGE_ALIGN_DOWN(p->p_vaddr);
        uintptr_t last = PAGE_ALIGN_UP(p->p_vaddr + p->p_memsz);
        uintptr_t vma_start = first < mapped_end ? mapped_end : first;

        for (uintptr_t pg = first; pg < last; pg += PAGE_SIZE) {
            uintptr_t lo = pg < p->p_vaddr ? p->p_vaddr : pg;
            uintptr_t file_end = (uintptr_t)p->p_vaddr + p->p_filesz;
            uintptr_t hi = pg + PAGE_SIZE < file_end ? pg + PAGE_SIZE : file_end;
            size_t n = hi > lo ? hi - lo : 0;
            int r = load_page(mm->pgd_phys, pg, pg < mapped_end, flags,
                              base + p->p_offset + (lo - p->p_vaddr), n, lo - pg);
            if (r) return r;
        }

        if (vma_start < last) {
            uint32_t vf = VM_USER | VM_ANONYMOUS | VM_READ |
                          ((p->p_flags & PF_W) ? VM_WRITE : 0) | ((p->p_flags & PF_X) ? VM_EXEC : 0);
            if (!insert_vma(mm, vma_start, last, vf, flags, NULL, 0)) return -ENOMEM;
        }
        mapped_end = last;
    }
    return 0;
}
