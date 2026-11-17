/**
 * @file syscall_proc.c
 * @brief Process system calls: exit, fork, execve, wait, kill, ids, sessions.
 */

#include "syscall_internal.h"
#include <kernel/process/scheduler.h>
#include <kernel/process/signal.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define WUNTRACED   2
#define RUSAGE_SIZE 72                  /* struct rusage on i386 */

long sc_exit(uint32_t code) {
    pcb_t *p = get_current_process();

    process_close_fds(p);
    process_exit_with_status(p, (code & 0xff) << 8);    /* Linux wait status layout */
    remove_current_task_with_code(code & 0xff);
    return 0;                                           /* not reached */
}

long sc_fork(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, isr_frame_t *regs) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return process_fork(regs);
}

long sc_execve(uint32_t path, uint32_t argv, uint32_t envp, uint32_t d, uint32_t e, uint32_t f, isr_frame_t *regs) {
    char kpath[PATH_MAX_SYS];
    struct exec_args args;
    (void)d; (void)e; (void)f;

    long err = copy_path_from_user((const char *)path, kpath, sizeof(kpath));
    if (err) return err;
    err = exec_args_copy(argv, envp, &args);
    if (err) return err;

    err = process_execve(kpath, args.argv, args.envp, regs);
    exec_args_free(&args);
    return err;
}

long sc_wait4(int32_t pid, uint32_t ustatus, uint32_t options, uint32_t rusage) {
    pcb_t *p = get_current_process();
    int want = pid > 0 ? pid : -1;      /* process groups are not tracked: 0 and < -1 mean any child */

    if (options & ~(uint32_t)(WNOHANG | WUNTRACED)) return -EINVAL;
    if (ustatus && !access_ok(VERIFY_WRITE, (void *)ustatus, sizeof(int))) return -EFAULT;
    if (rusage) {
        char zero[RUSAGE_SIZE] = {0};
        if (copy_to_user((void *)rusage, zero, sizeof(zero)) != 0) return -EFAULT;
    }

    for (;;) {
        int status = 0;
        int reaped = process_reap_child(p, want, &status);

        if (reaped > 0) {
            if (ustatus)
                copy_to_user((void *)ustatus, &status, sizeof(status));   /* checked above */
            return reaped;
        }
        if (want > 0 ? !process_find_child(p, want) : !p->children)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        yield();                        /* children exist but none has exited yet */
    }
}

long sc_waitpid(int32_t pid, uint32_t status, uint32_t options) {
    return sc_wait4(pid, status, options, 0);
}

long sc_kill(int32_t pid, uint32_t sig) {
    pcb_t *p = get_current_process();

    if (pid <= 0) return -ENOSYS;                       /* process groups */
    if (sig >= SIGNAL_MAX) return -EINVAL;
    if (sig == 0) return process_get_by_pid(pid) ? 0 : -ESRCH;
    return signal_send(pid, sig, p->pid);
}

long sc_getpid(void)  { return get_current_process()->pid; }
long sc_gettid(void)  { return get_current_process()->pid; }
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
    return pid == 0 || pid == p->pid ? p : process_get_by_pid(pid);
}

long sc_setsid(void) { return process_setsid(get_current_process()); }

long sc_getsid(uint32_t pid) {
    pcb_t *t = target_process(pid);
    return t ? (long)process_getsid(t) : -ESRCH;
}

long sc_setpgid(uint32_t pid, uint32_t pgid) {
    pcb_t *t = target_process(pid);
    return t ? process_setpgid(t, pgid ? pgid : t->pid) : -ESRCH;
}

long sc_getpgid(uint32_t pid) {
    pcb_t *t = target_process(pid);
    return t ? (long)process_getpgid(t) : -ESRCH;
}

long sc_getpgrp(void) { return process_getpgid(get_current_process()); }
