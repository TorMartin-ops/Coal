/* Private to the system call layer: helpers shared by syscall_*.c. */
#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include <kernel/cpu/syscall.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/memory/uaccess.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>

#define ARG_MAX_BYTES   32768       /* total size of argv + envp strings */
#define ARG_MAX_VECS    256         /* argv + envp entries incl. terminators */

void proc_exec_reset(pcb_t *p);   /* handlers back to default, close-on-exec files closed */

/* Copy a NUL-terminated path from user space. 0, -EFAULT, -ENAMETOOLONG or -ENOENT (empty). */
int copy_path_from_user(const char *user, char *kbuf, size_t size);
/* Same, then made absolute against the process cwd and normalized. */
int user_path(uintptr_t user, char *out, size_t size);

/* argv/envp copied once into kernel memory; free with exec_args_free. */
struct exec_args {
    char  *block;
    char **argv;
    char **envp;
};
long exec_args_copy(uintptr_t user_argv, uintptr_t user_envp, struct exec_args *out);
void exec_args_free(struct exec_args *a);


#define DECL(...) long __VA_ARGS__;
#define user_regs() user_regs_of(get_current_process())

/* Handlers; syscall.c refers to them. */
DECL(sc_read(uint32_t, uintptr_t, size_t))
DECL(sc_write(uint32_t, uintptr_t, size_t))
DECL(sc_open(uintptr_t, uint32_t, uint32_t))
DECL(sc_openat(int, uintptr_t, uint32_t, uint32_t))
DECL(sc_close(uint32_t))
DECL(sc_lseek(uint32_t, int64_t, uint32_t))
DECL(sc_readv(uint32_t, uintptr_t, unsigned long))
DECL(sc_writev(uint32_t, uintptr_t, unsigned long))
DECL(sc_dup(uint32_t))
DECL(sc_dup2(uint32_t, uint32_t))
DECL(sc_pipe(uintptr_t))
DECL(sc_pipe2(uintptr_t, uint32_t))
DECL(sc_ioctl(uint32_t, uint32_t, uintptr_t))
DECL(sc_fcntl(uint32_t, uint32_t, unsigned long))
DECL(sc_poll(uintptr_t, unsigned long, int32_t))
DECL(sc_select(int, uintptr_t, uintptr_t, uintptr_t, uintptr_t))
DECL(sc_pselect6(int, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))
DECL(sc_access(uintptr_t, uint32_t))
DECL(sc_statfs(uintptr_t, uintptr_t))
DECL(sc_fstatfs(uint32_t, uintptr_t))
DECL(sc_link(uintptr_t, uintptr_t))
DECL(sc_symlink(uintptr_t, uintptr_t))
DECL(sc_readlink(uintptr_t, uintptr_t, long))
DECL(sc_mkdir(uintptr_t, uint32_t))
DECL(sc_rmdir(uintptr_t))
DECL(sc_unlink(uintptr_t))
DECL(sc_rename(uintptr_t, uintptr_t))
DECL(sc_utime(uintptr_t, uintptr_t))
DECL(sc_sync(void))
DECL(sc_fsync(uint32_t))
DECL(sc_mount(uintptr_t, uintptr_t, uintptr_t))
DECL(sc_umount2(uintptr_t, uint32_t))
DECL(sc_chdir(uintptr_t))
DECL(sc_getcwd(uintptr_t, size_t))
DECL(sc_stat(uintptr_t, uintptr_t))
DECL(sc_fstat(uint32_t, uintptr_t))
DECL(sc_newfstatat(int, uintptr_t, uintptr_t, uint32_t))
DECL(sc_getdents64(uint32_t, uintptr_t, size_t))

DECL(sc_exit(int))
DECL(sc_fork(void))
DECL(sc_clone(uint64_t flags, uintptr_t stack, uintptr_t ptid, uintptr_t ctid, uintptr_t tls))
DECL(sc_execve(uintptr_t, uintptr_t, uintptr_t))
DECL(sc_wait4(int32_t, uintptr_t, uint32_t, uintptr_t))
DECL(sc_getpid(void))
DECL(sc_getppid(void))
DECL(sc_kill(int32_t, int))
DECL(sc_tkill(int32_t, int))
DECL(sc_tgkill(int32_t, int32_t, int))
DECL(sc_setsid(void))
DECL(sc_getsid(uint32_t))
DECL(sc_setpgid(uint32_t, uint32_t))
DECL(sc_getpgid(uint32_t))
DECL(sc_getpgrp(void))
DECL(sc_sched_yield(void))
DECL(sc_getpriority(int, uint32_t))
DECL(sc_setpriority(int, uint32_t, int))
DECL(sc_getrlimit(uint32_t, uintptr_t))
DECL(sc_setrlimit(uint32_t, uintptr_t))
DECL(sc_prlimit64(uint32_t, uint32_t, uintptr_t, uintptr_t))

DECL(sc_brk(uintptr_t))
DECL(sc_mmap(uintptr_t, size_t, uint32_t, uint32_t, int, long))
DECL(sc_munmap(uintptr_t, size_t))
DECL(sc_mprotect(uintptr_t, size_t, uint32_t))
DECL(sc_madvise(uintptr_t, size_t, int))

DECL(sc_time(uintptr_t))
DECL(sc_gettimeofday(uintptr_t, uintptr_t))
DECL(sc_clock_gettime(uint32_t, uintptr_t))
DECL(sc_clock_getres(uint32_t, uintptr_t))
DECL(sc_clock_nanosleep(uint32_t clk, uint32_t flags, uintptr_t ureq, uintptr_t urem))
DECL(sc_nanosleep(uintptr_t, uintptr_t))
DECL(sc_uname(uintptr_t))
DECL(sc_zero(void))                 /* getuid and friends: always root */
DECL(sc_reboot(uint32_t, uint32_t, uint32_t))
DECL(sc_syslog(int, uintptr_t, size_t))
DECL(sc_sysinfo(uintptr_t))
DECL(sc_arch_prctl(int, uintptr_t))
DECL(sc_set_tid_address(uintptr_t))
DECL(sc_getrandom(uintptr_t, size_t, uint32_t))
DECL(sc_rt_sigreturn(void))
DECL(sc_sigaltstack(uintptr_t, uintptr_t))
DECL(sc_rt_sigaction(uint32_t, uintptr_t, uintptr_t, size_t))
DECL(sc_rt_sigprocmask(uint32_t, uintptr_t, uintptr_t, size_t))

/* syscall_socket.c */
DECL(sc_socket(uint32_t, uint32_t, uint32_t))
DECL(sc_socketpair(uint32_t, uint32_t, uint32_t, uintptr_t))
DECL(sc_bind(uint32_t, uintptr_t, uint32_t))
DECL(sc_connect(uint32_t, uintptr_t, uint32_t))
DECL(sc_listen(uint32_t, uint32_t))
DECL(sc_accept(uint32_t, uintptr_t, uintptr_t))
DECL(sc_accept4(uint32_t, uintptr_t, uintptr_t, uint32_t))
DECL(sc_getsockopt(uint32_t, int, int, uintptr_t, uintptr_t))
DECL(sc_setsockopt(uint32_t, int, int, uintptr_t, uint32_t))
DECL(sc_getsockname(uint32_t, uintptr_t, uintptr_t))
DECL(sc_getpeername(uint32_t, uintptr_t, uintptr_t))
DECL(sc_sendto(uint32_t, uintptr_t, size_t, uint32_t, uintptr_t, uint32_t))
DECL(sc_sendmsg(uint32_t, uintptr_t, uint32_t))
DECL(sc_recvfrom(uint32_t, uintptr_t, size_t, uint32_t, uintptr_t, uintptr_t))
DECL(sc_recvmsg(uint32_t, uintptr_t, uint32_t))
DECL(sc_shutdown(uint32_t, uint32_t))

#endif
