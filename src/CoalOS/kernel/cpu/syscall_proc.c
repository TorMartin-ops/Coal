/**
 * @file syscall_proc.c
 * @brief Process system calls: exit, fork, execve, wait, kill, ids, sessions.
 */

#include "syscall_internal.h"
#include <kernel/process/scheduler.h>
#include <kernel/cpu/gdt.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define WUNTRACED   2
#define RUSAGE_SIZE 72                  /* struct rusage on i386 */

long sc_exit(uint32_t code) {
    process_exit(PROC_EXITCODE(code));
}

long sc_fork(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, isr_frame_t *regs) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return process_fork(regs);
}

long sc_execve(uint32_t path, uint32_t argv, uint32_t envp, uint32_t d, uint32_t e, uint32_t f, isr_frame_t *regs) {
    char kpath[PATH_MAX_SYS];
    struct exec_args args;
    (void)d; (void)e; (void)f;

    long err = user_path(path, kpath, sizeof(kpath));
    if (err) return err;
    err = exec_args_copy(argv, envp, &args);
    if (err) return err;

    err = process_execve(kpath, (const char *const *)args.argv, (const char *const *)args.envp, regs);
    exec_args_free(&args);
    if (err == 0) {
        proc_exec_reset(get_current_process());
        return regs->eax;               /* the new image's initial eax */
    }
    return err;
}

void proc_exec_reset(pcb_t *p) {
    memset(p->sigact, 0, sizeof(p->sigact));       /* handlers do not survive execve */
    for (int fd = 0; fd < MAX_FD; fd++)
        if (p->cloexec & (1u << fd)) sc_close(fd);
    p->cloexec = 0;
    p->tls_base = p->tls_pages = 0;
    gdt_set_tls_base(0, 0);
}

long sc_wait4(int32_t pid, uint32_t ustatus, uint32_t options, uint32_t rusage) {
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

long sc_waitpid(int32_t pid, uint32_t status, uint32_t options) {
    return sc_wait4(pid, status, options, 0);
}

long sc_kill(int32_t pid, uint32_t sig) {
    return process_kill(pid, sig);
}

long sc_getpid(void)  { return get_current_process()->pid; }
long sc_getppid(void) { return get_current_process()->ppid; }

long sc_sched_yield(void) {
    yield();
    return 0;
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
    pcb_t *t = target_process(pid);
    return t ? (long)t->sid : -ESRCH;
}

long sc_setpgid(uint32_t pid, uint32_t pgid) {
    pcb_t *t = target_process(pid);
    return t ? process_setpgid(t, pgid ? pgid : t->pid) : -ESRCH;
}

long sc_getpgid(uint32_t pid) {
    pcb_t *t = target_process(pid);
    return t ? (long)t->pgid : -ESRCH;
}

long sc_getpgrp(void) { return get_current_process()->pgid; }
