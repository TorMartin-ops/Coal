/**
 * @file mm.c
 * @brief Address spaces: VMA tree, demand paging, copy-on-write, brk/mmap/munmap.
 */

#include <kernel/memory/mm.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/frame.h>
#include <kernel/process/oom.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>

#define FAULT_AROUND 4                  /* pages mapped by one demand-zero fault, at most */

static vma_struct_t *vma_alloc(void) {
    vma_struct_t *v = kmalloc(sizeof(*v));
    if (v)
        memset(v, 0, sizeof(*v));
    return v;
}

/* PROT_NONE mappings stay present but become supervisor-only, so user accesses fault. */
static uint64_t prot_of(uint32_t vm_flags) {
    return PAGE_PRESENT | ((vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) ? PAGE_USER : 0) |
           ((vm_flags & VM_WRITE) ? PAGE_RW : 0) | ((vm_flags & VM_EXEC) ? 0 : PAGE_NX) |
           ((vm_flags & VM_SHARED) ? PAGE_SHARED : 0);
}

static bool user_range_ok(uintptr_t start, uintptr_t end) {
    return start < end && start >= USER_SPACE_MIN && end <= USER_SPACE_MAX;
}

/* --- Tree helpers (mm->lock held) ------------------------------------------ */

static void vma_link(mm_struct_t *mm, vma_struct_t *v) {
    struct rb_node **link = &mm->vma_tree.root;
    struct rb_node *parent = NULL;
    bool left = true;
    while (*link) {
        parent = *link;
        left = v->vm_start < rb_entry(parent, vma_struct_t, rb_node)->vm_start;
        link = left ? &parent->left : &parent->right;
    }
    rb_tree_insert_at(&mm->vma_tree, parent, &v->rb_node, left);
    v->vm_mm = mm;
    mm->map_count++;
}

static void vma_unlink(mm_struct_t *mm, vma_struct_t *v) {
    rb_tree_remove(&mm->vma_tree, &v->rb_node);
    mm->map_count--;
}

static vma_struct_t *overlap(mm_struct_t *mm, uintptr_t start, uintptr_t end) {
    return rbtree_find_overlap(mm->vma_tree.root, start, end);
}

static vma_struct_t *insert_locked(mm_struct_t *mm, uintptr_t start, uintptr_t end, uint32_t vm_flags) {
    if (overlap(mm, start, end))
        return NULL;
    vma_struct_t *v = vma_alloc();
    if (!v)
        return NULL;
    v->vm_start = start;
    v->vm_end = end;
    v->vm_flags = vm_flags;
    v->page_prot = prot_of(vm_flags);
    vma_link(mm, v);
    return v;
}

/* --- Lifetime ---------------------------------------------------------------- */

mm_struct_t *create_mm(void *pgd_phys) {
    mm_struct_t *mm = kmalloc(sizeof(*mm));
    if (!mm)
        return NULL;
    memset(mm, 0, sizeof(*mm));
    mm->pgd_phys = pgd_phys;
    rb_tree_init(&mm->vma_tree);
    spinlock_init(&mm->lock);
    return mm;
}

static void free_vma_cb(vma_struct_t *v, void *unused) {
    (void)unused;
    kfree(v);
}

void destroy_mm(mm_struct_t *mm) {
    if (!mm)
        return;
    rbtree_postorder_traverse(mm->vma_tree.root, free_vma_cb, NULL);
    paging_free_user_space(mm->pgd_phys);
    kfree(mm);
}

vma_struct_t *insert_vma(mm_struct_t *mm, uintptr_t start, uintptr_t end, uint32_t vm_flags) {
    if (!mm || ((start | end) & PAGING_OFFSET_MASK) || !user_range_ok(start, end))
        return NULL;
    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    vma_struct_t *v = insert_locked(mm, start, end, vm_flags);
    spinlock_release_irqrestore(&mm->lock, flags);
    return v;
}

size_t mm_total_vm(mm_struct_t *mm) {
    size_t total = 0;
    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    for (struct rb_node *n = rb_tree_first(&mm->vma_tree); n; n = rb_node_next(n)) {
        vma_struct_t *v = rb_entry(n, vma_struct_t, rb_node);
        total += v->vm_end - v->vm_start;
    }
    spinlock_release_irqrestore(&mm->lock, flags);
    return total;
}

/* Shared pages must exist before the fork so that both sides see the same frame. */
static bool populate_shared(mm_struct_t *mm) {
    for (struct rb_node *n = rb_tree_first(&mm->vma_tree); n; n = rb_node_next(n)) {
        vma_struct_t *v = rb_entry(n, vma_struct_t, rb_node);
        if (!(v->vm_flags & VM_SHARED))
            continue;
        for (uintptr_t a = v->vm_start; a < v->vm_end; a += PAGE_SIZE) {
            pte_t *pte = paging_pte(mm->pgd_phys, a, true);
            if (!pte)
                return false;
            if (*pte & PAGE_PRESENT)
                continue;
            uintptr_t frame = frame_alloc_user_zeroed();
            if (!frame)
                return false;
            *pte = frame | v->page_prot;
        }
    }
    return true;
}

mm_struct_t *dup_mm(mm_struct_t *parent) {
    uintptr_t flags = spinlock_acquire_irqsave(&parent->lock);

    if (!populate_shared(parent)) {
        spinlock_release_irqrestore(&parent->lock, flags);
        return NULL;
    }

    uintptr_t pgd = paging_clone_directory(parent->pgd_phys);
    mm_struct_t *child = pgd ? create_mm((void *)pgd) : NULL;
    if (!child) {
        spinlock_release_irqrestore(&parent->lock, flags);
        if (pgd) {
            paging_free_user_space((void *)pgd);
            put_frame(pgd);
        }
        return NULL;
    }

    child->start_code = parent->start_code;   child->end_code = parent->end_code;
    child->start_data = parent->start_data;   child->end_data = parent->end_data;
    child->start_brk = parent->start_brk;     child->end_brk = parent->end_brk;
    child->start_stack = parent->start_stack;

    for (struct rb_node *n = rb_tree_first(&parent->vma_tree); n; n = rb_node_next(n)) {
        vma_struct_t *pv = rb_entry(n, vma_struct_t, rb_node);
        vma_struct_t *cv = insert_locked(child, pv->vm_start, pv->vm_end, pv->vm_flags);
        if (!cv) {
            spinlock_release_irqrestore(&parent->lock, flags);
            destroy_mm(child);
            put_frame(pgd);
            return NULL;
        }
    }
    spinlock_release_irqrestore(&parent->lock, flags);
    return child;
}

/* --- munmap / brk / mmap ------------------------------------------------------ */

int remove_vma_range(mm_struct_t *mm, uintptr_t start, size_t length) {
    uintptr_t end = start + PAGE_ALIGN_UP(length);
    if (!mm || length == 0 || (start & PAGING_OFFSET_MASK) || end < start ||
        !user_range_ok(start, end))
        return -EINVAL;

    /* A VMA strictly containing the range splits in two; reserve the second
     * half up front so that nothing can fail after the first change. */
    vma_struct_t *spare = vma_alloc();
    if (!spare)
        return -ENOMEM;

    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    vma_struct_t *v;
    while ((v = overlap(mm, start, end)) != NULL) {
        if (v->vm_start >= start && v->vm_end <= end) {
            vma_unlink(mm, v);
            kfree(v);
        } else if (v->vm_start < start && v->vm_end > end) {
            *spare = *v;
            spare->vm_start = end;
            v->vm_end = start;
            vma_link(mm, spare);
            spare = NULL;
        } else if (v->vm_start < start) {
            v->vm_end = start;
        } else {
            v->vm_start = end;
        }
    }
    spinlock_release_irqrestore(&mm->lock, flags);

    kfree(spare);
    return paging_unmap_range(mm->pgd_phys, start, end - start) ? -EINVAL : 0;
}

/* Splits v at addr (inside it): v keeps the lower part, `upper` becomes the higher one. */
static void vma_split(mm_struct_t *mm, vma_struct_t *v, uintptr_t addr, vma_struct_t *upper) {
    *upper = *v;
    upper->vm_start = addr;
    v->vm_end = addr;
    vma_link(mm, upper);
}

int mm_protect(mm_struct_t *mm, uintptr_t start, size_t length, uint32_t prot_flags) {
    uintptr_t end = start + PAGE_ALIGN_UP(length);
    if (!mm || (start & PAGING_OFFSET_MASK) || end < start || !user_range_ok(start, end))
        return -EINVAL;
    if (length == 0)
        return 0;

    vma_struct_t *spare[2] = { vma_alloc(), vma_alloc() };      /* one per boundary that may split a VMA */
    int rc = spare[0] && spare[1] ? 0 : -ENOMEM;
    uint64_t prot = 0;

    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    for (uintptr_t a = start; !rc && a < end;) {                /* the range must be fully mapped */
        vma_struct_t *v = rbtree_find_vma(mm->vma_tree.root, a);
        if (!v)
            rc = -ENOMEM;
        else
            a = v->vm_end;
    }
    if (!rc) {
        vma_struct_t *v = rbtree_find_vma(mm->vma_tree.root, start);
        if (v->vm_start < start)
            vma_split(mm, v, start, spare[0]), spare[0] = NULL;
        v = rbtree_find_vma(mm->vma_tree.root, end - 1);
        if (v->vm_end > end)
            vma_split(mm, v, end, spare[1]), spare[1] = NULL;

        for (uintptr_t a = start; a < end;) {
            v = rbtree_find_vma(mm->vma_tree.root, a);
            v->vm_flags = (v->vm_flags & ~(uint32_t)(VM_READ | VM_WRITE | VM_EXEC)) | prot_flags;
            v->page_prot = prot_of(v->vm_flags);
            prot = v->page_prot;
            a = v->vm_end;
        }
    }
    spinlock_release_irqrestore(&mm->lock, flags);
    kfree(spare[0]);
    kfree(spare[1]);
    if (rc)
        return rc;

    /* Present pages get the new rights; copy-on-write pages stay read-only so that the write fault copies. */
    for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
        pte_t *pte = paging_pte(mm->pgd_phys, a, false);
        if (!pte || !(*pte & PAGE_PRESENT))
            continue;
        uint64_t keep = *pte & (PAGING_ADDR_MASK | PAGE_ACCESSED | PAGE_DIRTY | PAGE_COW);
        *pte = keep | ((*pte & PAGE_COW) ? prot & ~PAGE_RW : prot);
        paging_invalidate_page((void *)a);
    }
    return 0;
}

uintptr_t mm_brk(mm_struct_t *mm, uintptr_t new_brk) {
    if (new_brk == 0 || new_brk < mm->start_brk || new_brk - mm->start_brk > MM_BRK_MAX)
        return mm->end_brk;

    uintptr_t heap = PAGE_ALIGN_UP(mm->start_brk);
    uintptr_t old_top = PAGE_ALIGN_UP(mm->end_brk);
    uintptr_t new_top = PAGE_ALIGN_UP(new_brk);
    if (new_top < heap)
        new_top = heap;

    if (new_top < old_top) {
        if (remove_vma_range(mm, new_top, old_top - new_top) != 0)
            return mm->end_brk;
    } else if (new_top > old_top) {
        uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
        bool ok = user_range_ok(heap, new_top) && !overlap(mm, old_top, new_top);
        if (ok) {
            vma_struct_t *v = (old_top > heap) ? rbtree_find_vma(mm->vma_tree.root, old_top - 1) : NULL;
            if (v)
                v->vm_end = new_top;
            else
                ok = insert_locked(mm, heap, new_top, VM_READ | VM_WRITE | VM_USER | VM_ANONYMOUS | VM_HEAP) != NULL;
        }
        spinlock_release_irqrestore(&mm->lock, flags);
        if (!ok)
            return mm->end_brk;
    }
    mm->end_brk = new_brk;
    return new_brk;
}

intptr_t mm_mmap_anon(mm_struct_t *mm, uintptr_t hint, size_t len, uint32_t vm_flags) {
    if (len == 0)
        return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (len > MM_MMAP_LIMIT - MM_MMAP_BASE)
        return -ENOMEM;

    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    uintptr_t addr = 0;

    if (hint && !(hint & PAGING_OFFSET_MASK) && user_range_ok(hint, hint + len) &&
        hint + len > hint && !overlap(mm, hint, hint + len)) {
        addr = hint;
    } else {
        /* First fit between the existing VMAs of the mmap window. */
        uintptr_t cursor = MM_MMAP_BASE;
        for (struct rb_node *n = rb_tree_first(&mm->vma_tree); n; n = rb_node_next(n)) {
            vma_struct_t *v = rb_entry(n, vma_struct_t, rb_node);
            if (v->vm_end <= cursor)
                continue;
            if (v->vm_start >= cursor + len)
                break;
            cursor = v->vm_end;
        }
        if (cursor + len <= MM_MMAP_LIMIT)
            addr = cursor;
    }

    vma_struct_t *v = addr ? insert_locked(mm, addr, addr + len,
                                           vm_flags | VM_USER | VM_ANONYMOUS) : NULL;
    spinlock_release_irqrestore(&mm->lock, flags);
    return v ? (intptr_t)addr : -ENOMEM;
}

/* --- Faults ------------------------------------------------------------------- */

/* Grow the stack VMA just above addr down to cover it. */
static vma_struct_t *grow_stack(mm_struct_t *mm, uintptr_t addr) {
    vma_struct_t *v = rbtree_find_first_above(mm->vma_tree.root, addr);
    uintptr_t low = PAGE_ALIGN_DOWN(addr);
    if (!v || !(v->vm_flags & VM_GROWS_DOWN) || low < USER_SPACE_MIN ||
        v->vm_end - low > MM_STACK_MAX)
        return NULL;
    v->vm_start = low;      /* no VMA lies between addr and v, so tree order is unchanged */
    return v;
}

/* After a demand-zero fault, also map the next absent pages of the VMA (up to `end`) that share the
 * page table. Sequential first touches (heap growth, big buffers) then cost one fault instead of
 * FAULT_AROUND. Takes only spare memory and never triggers the OOM killer. */
static void fault_around(pte_t *pte, uint64_t prot, uintptr_t page, uintptr_t end) {
    for (unsigned i = 1; i < FAULT_AROUND; i++) {
        page += PAGE_SIZE;
        if (page >= end || !PTE_INDEX(page) || (pte[i] & PAGE_PRESENT))
            return;
        uintptr_t frame = frame_alloc_user_zeroed_spare();
        if (!frame)
            return;
        pte[i] = frame | prot;
    }
}

/* Make the page at `page` present (and writable if asked) with PTE flags prot.
 * `around` is the end of the VMA when neighbours may be mapped too, else 0.
 * Touches page tables only, never the VMA tree, so it runs without mm->lock. */
static int resolve(mm_struct_t *mm, uint64_t prot, uintptr_t page, bool write, uintptr_t around) {
    pte_t *pte = paging_pte(mm->pgd_phys, page, true);
    if (!pte)
        return VM_FAULT_OOM;

    if (!(*pte & PAGE_PRESENT)) {                       /* demand-zero page */
        uintptr_t frame = frame_alloc_user_zeroed();
        if (!frame)
            return VM_FAULT_OOM;
        *pte = frame | prot;
        if (around)
            fault_around(pte, prot, page, around);
        return VM_FAULT_OK;
    }
    if (!write || (*pte & PAGE_RW))
        return VM_FAULT_OK;                             /* nothing to do (stale TLB) */
    if (!(*pte & PAGE_COW))
        return VM_FAULT_SEGV;

    uintptr_t shared = *pte & PAGING_ADDR_MASK;
    uint64_t flags = (*pte & PAGING_FLAG_MASK & ~PAGE_COW) | PAGE_RW;
    if (get_frame_refcount(shared) == 1) {              /* last owner: just reclaim it */
        *pte = shared | flags;
    } else {
        uintptr_t copy = frame_alloc_user();
        if (!copy)
            return VM_FAULT_OOM;
        frame_copy(copy, shared);
        *pte = copy | flags;
        put_frame(shared);
    }
    paging_invalidate_page((void *)page);
    return VM_FAULT_OK;
}

/* resolve(); if memory is short the OOM killer frees some and the allocation is tried again. */
static int resolve_or_kill(mm_struct_t *mm, uint64_t prot, uintptr_t page, bool write, uintptr_t around) {
    int r;

    for (int i = 0; (r = resolve(mm, prot, page, write, around)) == VM_FAULT_OOM && i < OOM_RETRIES && oom_make_room(); i++) { }
    return r;
}

static bool vma_allows(const vma_struct_t *v, bool write) {
    return !(v->vm_flags & VM_FILEBACKED) &&
           (write ? (v->vm_flags & VM_WRITE) : (v->vm_flags & (VM_READ | VM_EXEC | VM_WRITE)));
}

int mm_fault(mm_struct_t *mm, uintptr_t addr, uint32_t error_code) {
    if (addr >= USER_SPACE_MAX)
        return VM_FAULT_SEGV;
    bool write = error_code & PAGE_FAULT_WRITE;

    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    vma_struct_t *v = rbtree_find_vma(mm->vma_tree.root, addr);
    if (!v)
        v = grow_stack(mm, addr);
    bool ok = v && vma_allows(v, write);
    uint64_t prot = ok ? v->page_prot : 0;
    uintptr_t around = ok && !(v->vm_flags & VM_GROWS_DOWN) ? v->vm_end : 0;
    spinlock_release_irqrestore(&mm->lock, flags);

    return ok ? resolve_or_kill(mm, prot, PAGE_ALIGN_DOWN(addr), write, around) : VM_FAULT_SEGV;
}

bool mm_access_ok(mm_struct_t *mm, uintptr_t addr, size_t len, bool write, bool populate) {
    if (len == 0)
        return true;
    uintptr_t end = addr + len;
    if (end < addr || end > USER_SPACE_MAX)
        return false;

    for (uintptr_t a = addr; a < end;) {
        uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
        vma_struct_t *v = rbtree_find_vma(mm->vma_tree.root, a);
        bool ok = v && vma_allows(v, write);
        uintptr_t seg_end = ok ? (v->vm_end < end ? v->vm_end : end) : 0;
        uint64_t prot = ok ? v->page_prot : 0;
        spinlock_release_irqrestore(&mm->lock, flags);
        if (!ok)
            return false;

        if (populate) {
            for (uintptr_t p = PAGE_ALIGN_DOWN(a); p < seg_end; p += PAGE_SIZE) {
                pte_t *pte = paging_pte(mm->pgd_phys, p, false);
                if ((!pte || !(*pte & PAGE_PRESENT) || (write && !(*pte & PAGE_RW))) &&
                    resolve_or_kill(mm, prot, p, write, 0) != VM_FAULT_OK)
                    return false;
            }
        }
        a = seg_end;
    }
    return true;
}
