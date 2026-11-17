/**
 * Process life cycle: creation, fork, exit, wait, kernel stacks, open files.
 * exec and the user stack live in exec.c, signals in signal.c.
 */
#include <kernel/process/process.h>
#include <kernel/process/fpu.h>
#include <kernel/process/exec.h>
#include <kernel/process/signal.h>
#include <kernel/drivers/wait.h>
#include <kernel/lib/assert.h>
#include <kernel/cpu/gdt.h>
#include <kernel/drivers/display/console_dev.h>
#include <kernel/fs/procfs.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/lib/string.h>
#include <kernel/memory/frame.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/mm.h>
#include <kernel/memory/paging_process.h>

#define USER_EFLAGS 0x202                 /* IF set */

/* Kernel stacks: a window (paging.h) of slots, each [guard page][stack pages].
 * The guard page is never mapped. The window's page tables exist from boot
 * and are shared by every address space. */
#define KSTACK_VA_START KERNEL_STACK_VADDR_START
#define KSTACK_VA_END   KERNEL_STACK_VADDR_END
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

static bool kstack_alloc(pcb_t *p) {
    uint32_t *kpd = (uint32_t *)g_kernel_page_directory_phys;
    uintptr_t va, flags = local_irq_save();

    if (kstack_nfree) va = kstack_free[--kstack_nfree];
    else if (kstack_next + KSTACK_SLOT <= KSTACK_VA_END) { va = kstack_next; kstack_next += KSTACK_SLOT; }
    else va = 0;
    local_irq_restore(flags);
    if (!va) return false;

    for (int i = 0; i < KSTACK_PAGES; i++) {
        uintptr_t f = frame_alloc();

        if (f && paging_map_single_4k(kpd, va + PAGE_SIZE * (i + 1), f, PTE_KERNEL_DATA_FLAGS)) {
            put_frame(f);
            f = 0;
        }
        if (!f) {
            if (i) paging_unmap_range(kpd, va + PAGE_SIZE, i * PAGE_SIZE);   /* drops the frames */
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
    paging_unmap_range(kpd, va + PAGE_SIZE, KSTACK_PAGES * PAGE_SIZE);   /* drops the frames */
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
    strcpy(p->cwd, "/");
    rlimit_init(p);
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
    fpu_release(p);
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

/** stdin, stdout and stderr share one open file on /dev/console. */
static void fds_init_std(pcb_t *p) {
    sys_file_t *sf = kmalloc(sizeof(*sf));

    if (!sf) return;
    memset(sf, 0, sizeof(*sf));
    sf->flags = O_RDWR;
    sf->vfs_file = create_console_file(O_RDWR);
    if (!sf->vfs_file) { kfree(sf); return; }
    sf->dups = 2;
    p->fd_table[0] = p->fd_table[1] = p->fd_table[2] = sf;
}

static void fds_close_all(pcb_t *p) {
    for (int i = 0; i < MAX_FD; i++) {
        uintptr_t flags = spinlock_acquire_irqsave(&p->fd_table_lock);
        sys_file_t *sf = p->fd_table[i];
        p->fd_table[i] = NULL;
        spinlock_release_irqrestore(&p->fd_table_lock, flags);
        if (sf) sys_file_put(sf);
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
    exec_set_name(p, path, argv);
    fds_init_std(p);
    process_init_user_frame(process_user_frame(p), img.entry, img.sp);
    process_set_entry_frame(p);
    pcb_publish(p, NULL);
    return p;
}

pcb_t *process_spawn_kernel(const char *name, void (*entry)(void)) {
    pcb_t *p = pcb_new();

    if (!p) return NULL;
    strncpy(p->name, name, sizeof(p->name) - 1);
    p->task.context = scheduler_make_context(p->kernel_stack_vaddr_top, entry, 0x202);
    pcb_publish(p, NULL);
    scheduler_add_task(p);
    return p;
}

int process_fork(const isr_frame_t *regs) {
    pcb_t *parent = get_current_process();
    pcb_t *child;
    mm_struct_t *mm;
    isr_frame_t *f;

    if (!parent || !parent->mm) return -ESRCH;
    if (!rlimit_fork_allowed(parent)) return -EAGAIN;
    child = pcb_new();
    if (!child) return -EAGAIN;
    mm = dup_mm(parent->mm);
    if (!mm) { kstack_free_pages(child); kfree(child); return -ENOMEM; }

    child->mm = mm;
    child->page_directory_phys = mm->pgd_phys;
    memcpy(child->name, parent->name, sizeof(child->name));
    memcpy(child->cmdline, parent->cmdline, sizeof(child->cmdline));
    child->cmdline_len = parent->cmdline_len;
    strcpy(child->cwd, parent->cwd);
    child->cloexec = parent->cloexec;
    rlimit_fork(child, parent);
    child->tls_base = parent->tls_base;
    child->tls_pages = parent->tls_pages;
    child->sig_blocked = parent->sig_blocked;
    fpu_fork(child, parent);
    memcpy(child->sigact, parent->sigact, sizeof(child->sigact));
    sys_files_clone(child, parent);
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

    if (!me) KERNEL_PANIC_HALT("process_exit without a current process");
    me->exiting = true;
    uintptr_t irq = local_irq_save();
    drv_wait_cancel(&me->task);           /* a signal can kill a task that still sits on a wait queue */
    local_irq_restore(irq);
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
        rlimit_exit(me);
        signal_send(me->parent, SIGCHLD);     /* ignored unless the parent handles it: wakes a poll() */
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
        if (signal_pending()) {
            local_irq_restore(flags);
            return -EINTR;
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

/* ---- /proc support ---------------------------------------------------------- */

int proc_info_get(uint32_t pid, proc_info_t *out) {
    uintptr_t flags = local_irq_save();
    pcb_t *p = process_find(pid);

    if (!p || !p->pid) { local_irq_restore(flags); return -ESRCH; }
    out->pid = p->pid;
    out->ppid = p->ppid;
    out->state = p->task.state == TASK_ZOMBIE ? 'Z'
               : p->task.state == TASK_RUNNING || p->task.state == TASK_READY ? 'R' : 'S';
    memcpy(out->name, p->name, sizeof(out->name));
    memcpy(out->cmdline, p->cmdline, sizeof(out->cmdline));
    out->cmdline_len = p->cmdline_len;
    out->rl_hits = p->rl_hits;
    local_irq_restore(flags);
    return 0;
}

int proc_info_list(uint32_t *pids, int max) {
    uintptr_t flags = local_irq_save();
    int n = 0;

    for (pcb_t *p = g_all; p && n < max; p = p->all_next)
        if (p->pid) pids[n++] = p->pid;
    local_irq_restore(flags);
    return n;
}
