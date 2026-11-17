/**
 * @file syscall.c
 * @brief The system call table and dispatcher (Linux i386 ABI).
 */

#include "syscall_internal.h"
#include <kernel/drivers/display/serial.h>

typedef long (*syscall_fn_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, isr_frame_t *);

/*
 * Handlers declare only the arguments they use. The i386 C calling convention
 * lets the dispatcher always pass all six registers plus the frame.
 */
#define SC(fn) ((syscall_fn_t)(void (*)(void))(fn))

static const syscall_fn_t syscall_table[NR_SYSCALLS] = {
    [SYS_EXIT]            = SC(sc_exit),
    [SYS_FORK]            = SC(sc_fork),
    [SYS_READ]            = SC(sc_read),
    [SYS_WRITE]           = SC(sc_write),
    [SYS_OPEN]            = SC(sc_open),
    [SYS_CLOSE]           = SC(sc_close),
    [SYS_WAITPID]         = SC(sc_waitpid),
    [SYS_UNLINK]          = SC(sc_unlink),
    [SYS_EXECVE]          = SC(sc_execve),
    [SYS_CHDIR]           = SC(sc_chdir),
    [SYS_TIME]            = SC(sc_time),
    [SYS_LSEEK]           = SC(sc_lseek),
    [SYS_GETPID]          = SC(sc_getpid),
    [SYS_GETUID]          = SC(sc_zero),
    [SYS_ACCESS]          = SC(sc_access),
    [SYS_KILL]            = SC(sc_kill),
    [SYS_MKDIR]           = SC(sc_mkdir),
    [SYS_RMDIR]           = SC(sc_rmdir),
    [SYS_DUP]             = SC(sc_dup),
    [SYS_PIPE]            = SC(sc_pipe),
    [SYS_BRK]             = SC(sc_brk),
    [SYS_GETGID]          = SC(sc_zero),
    [SYS_GETEUID]         = SC(sc_zero),
    [SYS_GETEGID]         = SC(sc_zero),
    [SYS_IOCTL]           = SC(sc_ioctl),
    [SYS_SETPGID]         = SC(sc_setpgid),
    [SYS_DUP2]            = SC(sc_dup2),
    [SYS_GETPPID]         = SC(sc_getppid),
    [SYS_GETPGRP]         = SC(sc_getpgrp),
    [SYS_SETSID]          = SC(sc_setsid),
    [SYS_GETTIMEOFDAY]    = SC(sc_gettimeofday),
    [SYS_MUNMAP]          = SC(sc_munmap),
    [SYS_WAIT4]           = SC(sc_wait4),
    [SYS_UNAME]           = SC(sc_uname),
    [SYS_GETPGID]         = SC(sc_getpgid),
    [SYS_LLSEEK]          = SC(sc_llseek),
    [SYS_READV]           = SC(sc_readv),
    [SYS_WRITEV]          = SC(sc_writev),
    [SYS_GETSID]          = SC(sc_getsid),
    [SYS_SCHED_YIELD]     = SC(sc_sched_yield),
    [SYS_NANOSLEEP]       = SC(sc_nanosleep),
    [SYS_RT_SIGACTION]    = SC(sc_rt_sigaction),
    [SYS_RT_SIGPROCMASK]  = SC(sc_rt_sigprocmask),
    [SYS_GETCWD]          = SC(sc_getcwd),
    [SYS_MMAP2]           = SC(sc_mmap2),
    [SYS_STAT64]          = SC(sc_stat64),
    [SYS_LSTAT64]         = SC(sc_stat64),      /* no symbolic links */
    [SYS_FSTAT64]         = SC(sc_fstat64),
    [SYS_GETUID32]        = SC(sc_zero),
    [SYS_GETGID32]        = SC(sc_zero),
    [SYS_GETEUID32]       = SC(sc_zero),
    [SYS_GETEGID32]       = SC(sc_zero),
    [SYS_GETDENTS64]      = SC(sc_getdents64),
    [SYS_GETTID]          = SC(sc_gettid),
    [SYS_SET_THREAD_AREA] = SC(sc_set_thread_area),
    [SYS_EXIT_GROUP]      = SC(sc_exit),
    [SYS_SET_TID_ADDRESS] = SC(sc_set_tid_address),
    [SYS_CLOCK_GETTIME]   = SC(sc_clock_gettime),
    [SYS_OPENAT]          = SC(sc_openat),
    [SYS_FSTATAT64]       = SC(sc_fstatat64),
    [SYS_PIPE2]           = SC(sc_pipe2),
};

void syscall_dispatch(isr_frame_t *r) {
    uint32_t nr = r->eax;
    syscall_fn_t fn = nr < NR_SYSCALLS ? syscall_table[nr] : NULL;

    if (fn)
        r->eax = (uint32_t)fn(r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp, r);
    else if (nr == SYS_READ_TERMINAL_LINE)
        r->eax = (uint32_t)sc_read_terminal_line(r->ebx, r->ecx);
    else
        r->eax = (uint32_t)-ENOSYS;
}

void syscall_init(void) {
    serial_write("[Syscall] Linux i386 ABI on int 0x80\n");
}
