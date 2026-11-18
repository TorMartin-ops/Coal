/**
 * @file syscall_proc.c
 * @brief Process system calls: exit, fork, execve, wait, kill, ids, sessions.
 */

#include "syscall_internal.h"
#include <kernel/sync/spinlock.h>
#include <kernel/process/scheduler.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define WUNTRACED   2
#define RUSAGE_SIZE 144                 /* struct rusage on x86_64 */

long sc_exit(int code) {
    process_exit(PROC_EXITCODE(code));
}

/* vfork is a fork: the child gets a copy-on-write copy, the parent just does not wait for it. */
long sc_fork(void) {
    return process_fork(user_regs());
}

/* The one clone libcs use for fork(): no shared address space, no new stack, SIGCHLD at exit. The child TID
 * stores are skipped (single-threaded: tgkill ignores the TID); threads and CLONE_VM give -ENOSYS. */
#define CLONE_PARENT_SETTID  0x00100000ul
#define CLONE_CHILD_CLEARTID 0x00200000ul
#define CLONE_CHILD_SETTID   0x01000000ul
#define CLONE_EXIT_SIGNAL    0xfful

long sc_clone(uint64_t flags, uintptr_t stack, uintptr_t ptid, uintptr_t ctid, uintptr_t tls) {
    (void)ctid;
    (void)tls;
    if ((flags & ~(CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_EXIT_SIGNAL)) ||
        (flags & CLONE_EXIT_SIGNAL) != SIGCHLD || stack)
        return -ENOSYS;
    long pid = process_fork(user_regs());
    if (pid > 0 && (flags & CLONE_PARENT_SETTID) && copy_to_user((void *)ptid, &(int32_t){ (int32_t)pid }, 4) != 0)
        return -EFAULT;
    return pid;
}

long sc_execve(uintptr_t path, uintptr_t argv, uintptr_t envp) {
    char kpath[PATH_MAX_SYS];
    struct exec_args args;
    isr_frame_t *regs = user_regs();

    long err = user_path(path, kpath, sizeof(kpath));
    if (err) return err;
    err = exec_args_copy(argv, envp, &args);
    if (err) return err;

    err = process_execve(kpath, (const char *const *)args.argv, (const char *const *)args.envp, regs);
    exec_args_free(&args);
    if (err == 0) {
        proc_exec_reset(get_current_process());
        return regs->rax;               /* the new image's initial rax */
    }
    return err;
}

void proc_exec_reset(pcb_t *p) {
    memset(p->sigact, 0, sizeof(p->sigact));       /* handlers do not survive execve */
    for (int fd = 0; fd < MAX_FD; fd++)
        if (p->cloexec & (1u << fd)) sc_close(fd);
    p->cloexec = 0;
    p->sigaltstack_sp = p->sigaltstack_size = 0;
}

long sc_wait4(int32_t pid, uintptr_t ustatus, uint32_t options, uintptr_t rusage) {
    int status = 0;

    if (options & ~(uint32_t)(WNOHANG | WUNTRACED)) return -EINVAL;
    if (ustatus && !access_ok(VERIFY_WRITE, (void *)ustatus, sizeof(status))) return -EFAULT;

    long r = process_waitpid(pid, &status, options & WNOHANG);
    if (r > 0 && ustatus && copy_to_user((void *)ustatus, &status, sizeof(status)) != 0)
        return -EFAULT;
    if (r > 0 && rusage) {
        char zero[RUSAGE_SIZE] = {0};
        if (copy_to_user((void *)rusage, zero, sizeof(zero)) != 0) return -EFAULT;
    }
    return r;
}

long sc_kill(int32_t pid, int sig) {
    return process_kill(pid, sig);
}

/* One thread per process: a thread id is a process id. */
long sc_tkill(int32_t tid, int sig) {
    return tid > 0 ? process_kill(tid, sig) : -EINVAL;
}

long sc_tgkill(int32_t tgid, int32_t tid, int sig) {
    return tgid == tid ? sc_tkill(tid, sig) : -ESRCH;
}

long sc_getpid(void)  { return get_current_process()->pid; }
long sc_getppid(void) { return get_current_process()->ppid; }

long sc_sched_yield(void) {
    yield();
    return 0;
}

#define PRIO_PROCESS 0

/* The kernel's getpriority returns 20 - nice, so that the result is never negative. */
long sc_getpriority(int which, uint32_t who) {
    pcb_t *p;

    if (which != PRIO_PROCESS) return -EINVAL;
    uintptr_t fl = local_irq_save();       /* a pcb found by pid can be freed under preemption */
    p = who ? process_find(who) : get_current_process();
    long r = p ? 20 - p->task.nice : -ESRCH;
    local_irq_restore(fl);
    return r;
}

long sc_setpriority(int which, uint32_t who, int nice) {
    pcb_t *p;

    if (which != PRIO_PROCESS) return -EINVAL;
    uintptr_t fl = local_irq_save();
    p = who ? process_find(who) : get_current_process();
    if (p) scheduler_set_nice(&p->task, nice);
    local_irq_restore(fl);
    return p ? 0 : -ESRCH;
}

//============================================================================
// Sessions and process groups
//============================================================================

static pcb_t *target_process(uint32_t pid) {
    pcb_t *p = get_current_process();
    return pid == 0 || pid == p->pid ? p : process_find(pid);
}

long sc_setsid(void) { return process_setsid(get_current_process()); }

long sc_getsid(uint32_t pid) {
    uintptr_t fl = local_irq_save();
    pcb_t *t = target_process(pid);
    long r = t ? (long)t->sid : -ESRCH;
    local_irq_restore(fl);
    return r;
}

long sc_setpgid(uint32_t pid, uint32_t pgid) {
    uintptr_t fl = local_irq_save();
    pcb_t *t = target_process(pid);
    long r = t ? process_setpgid(t, pgid ? pgid : t->pid) : -ESRCH;
    local_irq_restore(fl);
    return r;
}

long sc_getpgid(uint32_t pid) {
    uintptr_t fl = local_irq_save();
    pcb_t *t = target_process(pid);
    long r = t ? (long)t->pgid : -ESRCH;
    local_irq_restore(fl);
    return r;
}

long sc_getpgrp(void) { return get_current_process()->pgid; }

//============================================================================
// Resource limits (see rlimit.h)
//============================================================================

/* getrlimit, setrlimit and prlimit64 all use the 64-bit struct rlimit { cur, max }. */
long sc_prlimit64(uint32_t pid, uint32_t resource, uintptr_t unew, uintptr_t uold) {
    uint64_t nl[2], ol[2];

    if (unew && copy_from_user(nl, (const void *)unew, sizeof(nl)) != 0) return -EFAULT;
    uintptr_t fl = local_irq_save();        /* the target must not be freed between lookup and use */
    pcb_t *t = target_process(pid);
    long err = t ? rlimit_get(t, resource, &ol[0], &ol[1]) : -ESRCH;
    if (!err && unew) err = rlimit_set(t, resource, nl[0], nl[1]);
    local_irq_restore(fl);
    if (err) return err;
    if (uold && copy_to_user((void *)uold, ol, sizeof(ol)) != 0) return -EFAULT;
    return 0;
}

long sc_getrlimit(uint32_t resource, uintptr_t ulim) { return sc_prlimit64(0, resource, 0, ulim); }
long sc_setrlimit(uint32_t resource, uintptr_t ulim) { return sc_prlimit64(0, resource, ulim, 0); }
