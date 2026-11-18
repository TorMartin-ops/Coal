/**
 * @file syscall.c
 * @brief The system call table and dispatcher (Linux x86_64 ABI).
 */

#include "syscall_internal.h"
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/msr.h>

extern void syscall_entry(void);

/* IF so that a blocked call cannot starve the timer, TF, DF, AC (SMAP), NT, RF, VM. */
#define RFLAGS_SYSCALL_MASK 0x47700
/* SYSRET loads cs = STAR[63:48] + 16 and ss = STAR[63:48] + 8 (gdt.h). */
#define GDT_STAR_SYSRET     0x18

typedef long (*syscall_fn_t)(long, long, long, long, long, long);

/*
 * Handlers declare only the arguments they use, with the types the system call takes. The System V
 * calling convention lets the dispatcher always pass all six argument registers: a handler reads the
 * low bits of the ones it declares as 32-bit. Handlers that need the saved user registers (fork,
 * execve, signals) find them with user_regs().
 */
#define SC(fn) ((syscall_fn_t)(void (*)(void))(fn))

static const syscall_fn_t syscall_table[NR_SYSCALLS] = {
    [SYS_READ]              = SC(sc_read),
    [SYS_WRITE]             = SC(sc_write),
    [SYS_OPEN]              = SC(sc_open),
    [SYS_CLOSE]             = SC(sc_close),
    [SYS_STAT]              = SC(sc_stat),
    [SYS_FSTAT]             = SC(sc_fstat),
    [SYS_LSTAT]             = SC(sc_stat),
    [SYS_POLL]              = SC(sc_poll),
    [SYS_LSEEK]             = SC(sc_lseek),
    [SYS_MMAP]              = SC(sc_mmap),
    [SYS_MPROTECT]          = SC(sc_mprotect),
    [SYS_MUNMAP]            = SC(sc_munmap),
    [SYS_BRK]               = SC(sc_brk),
    [SYS_RT_SIGACTION]      = SC(sc_rt_sigaction),
    [SYS_RT_SIGPROCMASK]    = SC(sc_rt_sigprocmask),
    [SYS_RT_SIGRETURN]      = SC(sc_rt_sigreturn),
    [SYS_IOCTL]             = SC(sc_ioctl),
    [SYS_PREAD64]           = SC(sc_pread64),
    [SYS_PWRITE64]          = SC(sc_pwrite64),
    [SYS_READV]             = SC(sc_readv),
    [SYS_WRITEV]            = SC(sc_writev),
    [SYS_ACCESS]            = SC(sc_access),
    [SYS_PIPE]              = SC(sc_pipe),
    [SYS_SELECT]            = SC(sc_select),
    [SYS_SCHED_YIELD]       = SC(sc_sched_yield),
    [SYS_MADVISE]           = SC(sc_madvise),
    [SYS_DUP]               = SC(sc_dup),
    [SYS_DUP2]              = SC(sc_dup2),
    [SYS_DUP3]              = SC(sc_dup3),
    [SYS_NANOSLEEP]         = SC(sc_nanosleep),
    [SYS_GETPID]            = SC(sc_getpid),
    [SYS_SOCKET]            = SC(sc_socket),
    [SYS_CONNECT]           = SC(sc_connect),
    [SYS_ACCEPT]            = SC(sc_accept),
    [SYS_SENDTO]            = SC(sc_sendto),
    [SYS_RECVFROM]          = SC(sc_recvfrom),
    [SYS_SENDMSG]           = SC(sc_sendmsg),
    [SYS_RECVMSG]           = SC(sc_recvmsg),
    [SYS_SHUTDOWN]          = SC(sc_shutdown),
    [SYS_BIND]              = SC(sc_bind),
    [SYS_LISTEN]            = SC(sc_listen),
    [SYS_GETSOCKNAME]       = SC(sc_getsockname),
    [SYS_GETPEERNAME]       = SC(sc_getpeername),
    [SYS_SOCKETPAIR]        = SC(sc_socketpair),
    [SYS_SETSOCKOPT]        = SC(sc_setsockopt),
    [SYS_GETSOCKOPT]        = SC(sc_getsockopt),
    [SYS_CLONE]             = SC(sc_clone),
    [SYS_FORK]              = SC(sc_fork),
    [SYS_VFORK]             = SC(sc_fork),
    [SYS_EXECVE]            = SC(sc_execve),
    [SYS_EXIT]              = SC(sc_exit),
    [SYS_WAIT4]             = SC(sc_wait4),
    [SYS_KILL]              = SC(sc_kill),
    [SYS_UNAME]             = SC(sc_uname),
    [SYS_FCNTL]             = SC(sc_fcntl),
    [SYS_FSYNC]             = SC(sc_fsync),
    [SYS_FDATASYNC]         = SC(sc_fsync),
    [SYS_GETCWD]            = SC(sc_getcwd),
    [SYS_CHDIR]             = SC(sc_chdir),
    [SYS_RENAME]            = SC(sc_rename),
    [SYS_MKDIR]             = SC(sc_mkdir),
    [SYS_RMDIR]             = SC(sc_rmdir),
    [SYS_CREAT]             = SC(sc_creat),
    [SYS_TRUNCATE]          = SC(sc_truncate),
    [SYS_FTRUNCATE]         = SC(sc_ftruncate),
    [SYS_CHMOD]             = SC(sc_chmod),
    [SYS_FCHMOD]            = SC(sc_fchmod),
    [SYS_CHOWN]             = SC(sc_chown),
    [SYS_FCHOWN]            = SC(sc_fchown),
    [SYS_LCHOWN]            = SC(sc_chown),
    [SYS_UMASK]             = SC(sc_umask),
    [SYS_LINK]              = SC(sc_link),
    [SYS_UNLINK]            = SC(sc_unlink),
    [SYS_SYMLINK]           = SC(sc_symlink),
    [SYS_READLINK]          = SC(sc_readlink),
    [SYS_GETTIMEOFDAY]      = SC(sc_gettimeofday),
    [SYS_GETRLIMIT]         = SC(sc_getrlimit),
    [SYS_SYSINFO]           = SC(sc_sysinfo),
    [SYS_GETUID]            = SC(sc_getuid),
    [SYS_SYSLOG]            = SC(sc_syslog),
    [SYS_GETGID]            = SC(sc_getgid),
    [SYS_GETEUID]           = SC(sc_geteuid),
    [SYS_GETEGID]           = SC(sc_getegid),
    [SYS_SETUID]            = SC(sc_setuid),
    [SYS_SETGID]            = SC(sc_setgid),
    [SYS_SETREUID]          = SC(sc_setreuid),
    [SYS_SETREGID]          = SC(sc_setregid),
    [SYS_SETRESUID]         = SC(sc_setresuid),
    [SYS_GETRESUID]         = SC(sc_getresuid),
    [SYS_SETRESGID]         = SC(sc_setresgid),
    [SYS_GETRESGID]         = SC(sc_getresgid),
    [SYS_SETPGID]           = SC(sc_setpgid),
    [SYS_GETPPID]           = SC(sc_getppid),
    [SYS_GETPGRP]           = SC(sc_getpgrp),
    [SYS_SETSID]            = SC(sc_setsid),
    [SYS_GETPGID]           = SC(sc_getpgid),
    [SYS_GETSID]            = SC(sc_getsid),
    [SYS_SIGALTSTACK]       = SC(sc_sigaltstack),
    [SYS_UTIME]             = SC(sc_utime),
    [SYS_STATFS]            = SC(sc_statfs),
    [SYS_FSTATFS]           = SC(sc_fstatfs),
    [SYS_GETPRIORITY]       = SC(sc_getpriority),
    [SYS_SETPRIORITY]       = SC(sc_setpriority),
    [SYS_ARCH_PRCTL]        = SC(sc_arch_prctl),
    [SYS_SETRLIMIT]         = SC(sc_setrlimit),
    [SYS_SYNC]              = SC(sc_sync),
    [SYS_MOUNT]             = SC(sc_mount),
    [SYS_UMOUNT2]           = SC(sc_umount2),
    [SYS_REBOOT]            = SC(sc_reboot),
    [SYS_GETTID]            = SC(sc_getpid),
    [SYS_TKILL]             = SC(sc_tkill),
    [SYS_TIME]              = SC(sc_time),
    [SYS_GETDENTS64]        = SC(sc_getdents64),
    [SYS_SET_TID_ADDRESS]   = SC(sc_set_tid_address),
    [SYS_CLOCK_GETTIME]     = SC(sc_clock_gettime),
    [SYS_CLOCK_GETRES]      = SC(sc_clock_getres),
    [SYS_CLOCK_NANOSLEEP]   = SC(sc_clock_nanosleep),
    [SYS_EXIT_GROUP]        = SC(sc_exit),
    [SYS_TGKILL]            = SC(sc_tgkill),
    [SYS_OPENAT]            = SC(sc_openat),
    [SYS_NEWFSTATAT]        = SC(sc_newfstatat),
    [SYS_PSELECT6]          = SC(sc_pselect6),
    [SYS_ACCEPT4]           = SC(sc_accept4),
    [SYS_PIPE2]             = SC(sc_pipe2),
    [SYS_PRLIMIT64]         = SC(sc_prlimit64),
    [SYS_GETRANDOM]         = SC(sc_getrandom),
};

/* Calls that restart after a SA_RESTART handler (signal(7)). poll, nanosleep and select never do. */
static bool restartable(uint32_t nr) {
    switch (nr) {
    case SYS_READ: case SYS_WRITE: case SYS_READV: case SYS_WRITEV: case SYS_WAIT4:
    case SYS_ACCEPT: case SYS_ACCEPT4: case SYS_SENDTO: case SYS_SENDMSG: case SYS_RECVFROM: case SYS_RECVMSG:
        return true;
    default:
        return false;
    }
}

void syscall_dispatch(isr_frame_t *r) {
    uint64_t nr = r->rax;
    syscall_fn_t fn = nr < NR_SYSCALLS ? syscall_table[nr] : NULL;
    long rc = fn ? fn(r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9) : -ENOSYS;

    r->rax = rc;

    pcb_t *p = get_current_process();       /* the common case, no signal and no temporary mask, takes two loads */
    if (p->pending_signals) signal_return_to_user(r, rc == -EINTR && restartable(nr) ? (int)nr : SIGNAL_NO_RESTART);
    if (p->sig_restore) signal_mask_restore();
}

void syscall_init(void) {
    wrmsr(MSR_STAR, ((uint64_t)GDT_STAR_SYSRET << 48) | ((uint64_t)KERNEL_CODE_SELECTOR << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, RFLAGS_SYSCALL_MASK);
}
