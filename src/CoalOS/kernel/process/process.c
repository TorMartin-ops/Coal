/**
 * Process life cycle: creation, fork, exit, wait, kernel stacks, open files.
 * exec and the user stack live in exec.c, signals in signal.c.
 */
#define LOG_MODULE "process"
#include <kernel/process/process.h>
#include <kernel/process/exec.h>
#include <kernel/core/log.h>
#include <kernel/cpu/gdt.h>
#include <kernel/drivers/display/console_dev.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/lib/rbtree.h>
#include <kernel/lib/string.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/mm.h>
#include <kernel/memory/paging_core.h>
#include <kernel/memory/paging_process.h>

#define USER_EFLAGS 0x202                 /* IF set */

/* Kernel stacks: a 16 MiB window of slots, each [guard page][stack pages].
 * The guard page is never mapped. The window's page tables are created at boot
 * so every address space shares them (see kstack_init). */
#define KSTACK_VA_START 0xE0000000u
#define KSTACK_VA_END   0xE1000000u
#define KSTACK_SLOT     ((KSTACK_PAGES + 1) * PAGE_SIZE)
#define KSTACK_FREE_MAX 128

static uintptr_t kstack_next = KSTACK_VA_START;
static uintptr_t kstack_free[KSTACK_FREE_MAX];
static unsigned  kstack_nfree;

static pcb_t *g_all;                      /* every live process */
static pcb_t *g_dead;                     /* exited with no parent to wait for them */
static void dead_push(pcb_t *p) { p->task.next = (tcb_t *)g_dead; g_dead = p; }
static uint32_t g_next_pid = 1;

pcb_t *get_current_process(void) {
    tcb_t *t = get_current_task();
    return (t && t->process->pid) ? t->process : NULL;   /* pid 0 is the idle task */
}

/* ---- kernel stacks -------------------------------------------------- */

void process_kstack_init(void) {
    uint32_t *kpd = (uint32_t *)g_kernel_page_directory_phys;
    uintptr_t f = frame_alloc();

    if (!f) return;
    for (uintptr_t va = KSTACK_VA_START; va < KSTACK_VA_END; va += 4u << 20) {
        if (paging_map_single_4k(kpd, va, f, PTE_KERNEL_DATA_FLAGS) == 0)
            paging_unmap_range(kpd, va, PAGE_SIZE);
    }
    put_frame(f);
}

static bool kstack_alloc(pcb_t *p) {
    uint32_t *kpd = (uint32_t *)g_kernel_page_directory_phys;
    uintptr_t va, flags = local_irq_save();

    if (kstack_nfree) va = kstack_free[--kstack_nfree];
    else if (kstack_next + KSTACK_SLOT <= KSTACK_VA_END) { va = kstack_next; kstack_next += KSTACK_SLOT; }
    else va = 0;
    local_irq_restore(flags);
    if (!va) return false;

    for (int i = 0; i < KSTACK_PAGES; i++) {
        p->kstack_frames[i] = frame_alloc();
        if (!p->kstack_frames[i] ||
            paging_map_single_4k(kpd, va + PAGE_SIZE * (i + 1), p->kstack_frames[i], PTE_KERNEL_DATA_FLAGS)) {
            if (p->kstack_frames[i]) put_frame(p->kstack_frames[i]);
            while (i--) {
                paging_unmap_range(kpd, va + PAGE_SIZE * (i + 1), PAGE_SIZE);
                put_frame(p->kstack_frames[i]);
            }
            flags = local_irq_save();
            if (kstack_nfree < KSTACK_FREE_MAX) kstack_free[kstack_nfree++] = va;
            local_irq_restore(flags);
            return false;
        }
    }
    p->kernel_stack_vaddr_top = (uint32_t *)(va + KSTACK_SLOT);
    return true;
}

static void kstack_free_pages(pcb_t *p) {
    uint32_t *kpd = (uint32_t *)g_kernel_page_directory_phys;
    uintptr_t va = (uintptr_t)p->kernel_stack_vaddr_top - KSTACK_SLOT, flags;

    if (!p->kernel_stack_vaddr_top) return;
    paging_unmap_range(kpd, va + PAGE_SIZE, KSTACK_PAGES * PAGE_SIZE);
    for (int i = 0; i < KSTACK_PAGES; i++) put_frame(p->kstack_frames[i]);
    flags = local_irq_save();
    if (kstack_nfree < KSTACK_FREE_MAX) kstack_free[kstack_nfree++] = va;
    local_irq_restore(flags);
    p->kernel_stack_vaddr_top = NULL;
}

/** The frame the CPU/ret_from_fork uses for the first return to user mode. */
isr_frame_t *process_user_frame(pcb_t *p) {
    return (isr_frame_t *)p->kernel_stack_vaddr_top - 1;
}

/** Makes p runnable from a user frame already placed at the top of its kernel stack. */
void process_set_entry_frame(pcb_t *p) {
    p->task.context = scheduler_make_context(process_user_frame(p), ret_from_fork, 0x2);
}

void process_init_user_frame(isr_frame_t *f, uint32_t entry, uint32_t sp) {
    memset(f, 0, sizeof(*f));
    f->ds = f->es = f->fs = f->gs = GDT_USER_DATA_SELECTOR;
    f->cs = GDT_USER_CODE_SELECTOR;
    f->ss = GDT_USER_DATA_SELECTOR;
    f->eip = entry;
    f->useresp = sp;
    f->eflags = USER_EFLAGS;
}

/* ---- PCB bookkeeping -------------------------------------------------- */

static uint32_t alloc_pid_locked(void) {
    for (;;) {
        uint32_t pid = g_next_pid++;
        if (g_next_pid > 32767) g_next_pid = 2;
        pcb_t *p = g_all;
        while (p && p->pid != pid) p = p->all_next;
        if (!p) return pid;
    }
}

pcb_t *process_find(uint32_t pid) {
    pcb_t *p = g_all;
    while (p && p->pid != pid) p = p->all_next;
    return p;
}

pcb_t *process_all(void) {
    return g_all;
}

static pcb_t *pcb_new(void) {
    pcb_t *p = kmalloc(sizeof(*p));

    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    spinlock_init(&p->fd_table_lock);
    p->task.process = p;
    p->task.priority = SCHED_DEFAULT_PRIORITY;
    if (!kstack_alloc(p)) { kfree(p); return NULL; }
    return p;
}

/** Gives p a pid and links it into the process lists. */
static void pcb_publish(pcb_t *p, pcb_t *parent) {
    uintptr_t flags = local_irq_save();

    p->pid = alloc_pid_locked();
    p->all_next = g_all;
    g_all = p;
    if (parent) {
        p->parent = parent;
        p->ppid = parent->pid;
        p->pgid = parent->pgid;
        p->sid = parent->sid;
        p->sibling = parent->children;
        parent->children = p;
    } else {
        p->pgid = p->sid = p->pid;
    }
    local_irq_restore(flags);
}

static void unlink_from(pcb_t **list, pcb_t *p, size_t next_off) {
    while (*list && *list != p) list = (pcb_t **)((char *)*list + next_off);
    if (*list) *list = *(pcb_t **)((char *)p + next_off);
}

/** Final release of a process that has fully stopped running. */
static void pcb_free(pcb_t *p) {
    uintptr_t flags = local_irq_save();
    unlink_from(&g_all, p, offsetof(pcb_t, all_next));
    local_irq_restore(flags);
    kstack_free_pages(p);
    kfree(p);
}

void process_reap_orphans(void) {
    for (;;) {
        uintptr_t flags = local_irq_save();
        pcb_t *p = g_dead;
        if (p) g_dead = (pcb_t *)p->task.next;
        local_irq_restore(flags);
        if (!p) return;
        pcb_free(p);
    }
}

/* ---- open files --------------------------------------------------------- */

static void fds_init_std(pcb_t *p) {
    static const int modes[3] = { CONSOLE_STDIN_MODE, CONSOLE_STDOUT_MODE, CONSOLE_STDERR_MODE };

    for (int i = 0; i < 3; i++) {
        sys_file_t *sf = kmalloc(sizeof(*sf));
        if (!sf) return;
        memset(sf, 0, sizeof(*sf));
        sf->vfs_file = create_console_file(modes[i]);
        if (!sf->vfs_file) { kfree(sf); return; }
        p->fd_table[i] = sf;
    }
}

/** fork: the child shares every open-file object (and its offset) with the parent. */
static void fds_dup(pcb_t *dst, const pcb_t *src) {
    for (int i = 0; i < MAX_FD; i++)
        if (src->fd_table[i]) dst->fd_table[i] = sys_file_get(src->fd_table[i]);
}

static void fds_close_all(pcb_t *p) {
    for (int i = 0; i < MAX_FD; i++) {
        uintptr_t flags = spinlock_acquire_irqsave(&p->fd_table_lock);
        sys_file_t *sf = p->fd_table[i];
        p->fd_table[i] = NULL;
        spinlock_release_irqrestore(&p->fd_table_lock, flags);
        if (sf && sys_file_put(sf)) {
            vfs_close(sf->vfs_file);
            kfree(sf);
        }
    }
}

/* ---- creation ------------------------------------------------------------ */

pcb_t *create_user_process(const char *path) {
    const char *argv[2] = { path, NULL };
    const char *envp[1] = { NULL };
    exec_image_t img;
    pcb_t *p = pcb_new();

    if (!p) return NULL;
    if (exec_build_image(path, argv, envp, &img) != 0) { kstack_free_pages(p); kfree(p); return NULL; }
    p->mm = img.mm;
    p->page_directory_phys = img.mm->pgd_phys;
    fds_init_std(p);
    process_init_user_frame(process_user_frame(p), img.entry, img.sp);
    process_set_entry_frame(p);
    pcb_publish(p, NULL);
    LOG_DEBUG("created pid %u from %s", p->pid, path);
    return p;
}

static void clone_vma(vma_struct_t *v, void *arg) {
    struct { mm_struct_t *mm; bool ok; } *c = arg;
    if (c->ok && !insert_vma(c->mm, v->vm_start, v->vm_end, v->vm_flags, v->page_prot,
                             v->vm_file, v->vm_offset))
        c->ok = false;
}

/** Duplicates src's address space: page tables via paging_clone_directory, VMAs here. */
static mm_struct_t *mm_clone(const mm_struct_t *src) {
    uintptr_t pd = paging_clone_directory(src->pgd_phys);
    mm_struct_t *mm;
    struct { mm_struct_t *mm; bool ok; } c;

    if (!pd) return NULL;
    mm = create_mm((uint32_t *)pd);
    if (!mm) { paging_free_user_space((uint32_t *)pd); put_frame(pd); return NULL; }
    mm->start_code = src->start_code; mm->end_code = src->end_code;
    mm->start_data = src->start_data; mm->end_data = src->end_data;
    mm->start_brk = src->start_brk;   mm->end_brk = src->end_brk;
    mm->start_stack = src->start_stack;
    c.mm = mm; c.ok = true;
    rbtree_postorder_traverse(src->vma_tree.root, clone_vma, &c);
    if (!c.ok) { exec_free_mm(mm); return NULL; }
    return mm;
}

int process_fork(const isr_frame_t *regs) {
    pcb_t *parent = get_current_process();
    pcb_t *child;
    mm_struct_t *mm;
    isr_frame_t *f;

    if (!parent || !parent->mm) return -ESRCH;
    child = pcb_new();
    if (!child) return -EAGAIN;
    mm = mm_clone(parent->mm);
    if (!mm) { kstack_free_pages(child); kfree(child); return -ENOMEM; }

    child->mm = mm;
    child->page_directory_phys = mm->pgd_phys;
    fds_dup(child, parent);
    f = process_user_frame(child);
    *f = *regs;
    f->eax = 0;
    process_set_entry_frame(child);
    pcb_publish(child, parent);
    scheduler_add_task(child);
    return (int)child->pid;
}

/* ---- exit / wait ----------------------------------------------------------- */

void process_exit(int status) {
    pcb_t *me = get_current_process();
    pcb_t *init, *c;

    if (!me) for (;;) asm volatile("cli; hlt");
    fds_close_all(me);
    exec_release_mm(me);                  /* leaves the kernel page directory loaded */

    asm volatile("cli");
    me->exit_status = status;
    init = process_find(1);
    if (init == me) init = NULL;
    for (c = me->children; c; ) {         /* orphans go to init, or to nobody */
        pcb_t *next = c->sibling;
        c->parent = init;
        c->ppid = init ? 1 : 0;
        c->sibling = init ? init->children : NULL;
        if (init) init->children = c;
        if (c->task.state == TASK_ZOMBIE) {
            if (!init) dead_push(c);
            else if (init->wait_blocked) scheduler_unblock_task(&init->task);
        }
        c = next;
    }
    me->children = NULL;

    if (me->parent) {
        if (me->parent->wait_blocked) scheduler_unblock_task(&me->parent->task);
    } else {
        dead_push(me);
    }
    scheduler_exit_current();
}

static bool wait_matches(const pcb_t *me, const pcb_t *c, int pid) {
    if (pid > 0) return c->pid == (uint32_t)pid;
    if (pid == -1) return true;
    if (pid == 0) return c->pgid == me->pgid;
    return c->pgid == (uint32_t)-pid;
}

int process_waitpid(int pid, int *status, int options) {
    pcb_t *me = get_current_process();

    if (!me) return -ESRCH;
    if (options & ~WNOHANG) return -EINVAL;
    for (;;) {
        uintptr_t flags = local_irq_save();
        pcb_t *zombie = NULL;
        bool have_child = false;

        for (pcb_t *c = me->children; c; c = c->sibling) {
            if (!wait_matches(me, c, pid)) continue;
            have_child = true;
            if (c->task.state == TASK_ZOMBIE) { zombie = c; break; }
        }
        if (zombie) {
            pcb_t **pp = &me->children;
            while (*pp != zombie) pp = &(*pp)->sibling;
            *pp = zombie->sibling;
            local_irq_restore(flags);
            if (status) *status = zombie->exit_status;
            pid = (int)zombie->pid;
            pcb_free(zombie);
            return pid;
        }
        if (!have_child || (options & WNOHANG)) {
            local_irq_restore(flags);
            return have_child ? 0 : -ECHILD;
        }
        me->wait_blocked = true;
        me->task.state = TASK_BLOCKED;
        schedule();
        me->wait_blocked = false;
        local_irq_restore(flags);
    }
}

/* ---- sessions and groups --------------------------------------------------- */

int process_setsid(pcb_t *p) {
    if (p->pgid == p->pid && p->sid == p->pid) return -EPERM;   /* already a leader */
    p->sid = p->pgid = p->pid;
    return (int)p->sid;
}

int process_setpgid(pcb_t *p, uint32_t pgid) {
    if (!pgid) pgid = p->pid;
    if (p->sid == p->pid && p->pid != pgid) return -EPERM;      /* session leaders keep their group */
    p->pgid = pgid;
    return 0;
}
