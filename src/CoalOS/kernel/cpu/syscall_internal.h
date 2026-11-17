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
int user_path(uint32_t user, char *out, size_t size);

/* argv/envp copied once into kernel memory; free with exec_args_free. */
struct exec_args {
    char  *block;
    char **argv;
    char **envp;
};
long exec_args_copy(uint32_t user_argv, uint32_t user_envp, struct exec_args *out);
void exec_args_free(struct exec_args *a);


#define DECL(...) long __VA_ARGS__;
/* Handlers; syscall.c refers to them. */
DECL(sc_read(uint32_t, uint32_t, uint32_t))
DECL(sc_write(uint32_t, uint32_t, uint32_t))
DECL(sc_open(uint32_t, uint32_t, uint32_t))
DECL(sc_openat(int, uint32_t, uint32_t, uint32_t))
DECL(sc_close(uint32_t))
DECL(sc_lseek(uint32_t, int32_t, uint32_t))
DECL(sc_llseek(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_readv(uint32_t, uint32_t, uint32_t))
DECL(sc_writev(uint32_t, uint32_t, uint32_t))
DECL(sc_dup(uint32_t))
DECL(sc_dup2(uint32_t, uint32_t))
DECL(sc_pipe(uint32_t))
DECL(sc_pipe2(uint32_t, uint32_t))
DECL(sc_ioctl(uint32_t, uint32_t, uint32_t))
DECL(sc_fcntl64(uint32_t, uint32_t, uint32_t))
DECL(sc_poll(uint32_t, uint32_t, int32_t))
DECL(sc_oldselect(uint32_t))
DECL(sc_newselect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_pselect6(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_access(uint32_t, uint32_t))
DECL(sc_statfs(uint32_t, uint32_t))
DECL(sc_fstatfs(uint32_t, uint32_t))
DECL(sc_statfs64(uint32_t, uint32_t, uint32_t))
DECL(sc_fstatfs64(uint32_t, uint32_t, uint32_t))
DECL(sc_link(uint32_t, uint32_t))
DECL(sc_symlink(uint32_t, uint32_t))
DECL(sc_readlink(uint32_t, uint32_t, uint32_t))
DECL(sc_mkdir(uint32_t, uint32_t))
DECL(sc_rmdir(uint32_t))
DECL(sc_unlink(uint32_t))
DECL(sc_rename(uint32_t, uint32_t))
DECL(sc_utime(uint32_t, uint32_t))
DECL(sc_sync(void))
DECL(sc_fsync(uint32_t))
DECL(sc_mount(uint32_t, uint32_t, uint32_t))
DECL(sc_umount2(uint32_t, uint32_t))
DECL(sc_chdir(uint32_t))
DECL(sc_getcwd(uint32_t, uint32_t))
DECL(sc_stat64(uint32_t, uint32_t))
DECL(sc_fstat64(uint32_t, uint32_t))
DECL(sc_fstatat64(int, uint32_t, uint32_t, uint32_t))
DECL(sc_getdents64(uint32_t, uint32_t, uint32_t))

DECL(sc_exit(uint32_t))
DECL(sc_fork(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, isr_frame_t *))
DECL(sc_execve(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, isr_frame_t *))
DECL(sc_wait4(int32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_waitpid(int32_t, uint32_t, uint32_t))
DECL(sc_getpid(void))
DECL(sc_getppid(void))
DECL(sc_kill(int32_t, uint32_t))
DECL(sc_setsid(void))
DECL(sc_getsid(uint32_t))
DECL(sc_setpgid(uint32_t, uint32_t))
DECL(sc_getpgid(uint32_t))
DECL(sc_getpgrp(void))
DECL(sc_sched_yield(void))
DECL(sc_nice(int32_t))
DECL(sc_getpriority(uint32_t, uint32_t))
DECL(sc_setpriority(uint32_t, uint32_t, int32_t))
DECL(sc_setrlimit(uint32_t, uint32_t))
DECL(sc_ugetrlimit(uint32_t, uint32_t))
DECL(sc_prlimit64(uint32_t, uint32_t, uint32_t, uint32_t))

DECL(sc_brk(uint32_t))
DECL(sc_mmap2(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t))
DECL(sc_munmap(uint32_t, uint32_t))

DECL(sc_time(uint32_t))
DECL(sc_gettimeofday(uint32_t, uint32_t))
DECL(sc_clock_gettime(uint32_t, uint32_t))
DECL(sc_nanosleep(uint32_t, uint32_t))
DECL(sc_uname(uint32_t))
DECL(sc_zero(void))                 /* getuid and friends: always root */
DECL(sc_reboot(uint32_t, uint32_t, uint32_t))
DECL(sc_syslog(uint32_t, uint32_t, uint32_t))
DECL(sc_set_thread_area(uint32_t))
DECL(sc_set_tid_address(uint32_t))
DECL(sc_rt_sigreturn(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, isr_frame_t *))
DECL(sc_rt_sigaction(uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_rt_sigprocmask(uint32_t, uint32_t, uint32_t, uint32_t))

/* syscall_socket.c */
DECL(sc_socketcall(uint32_t, uint32_t))
DECL(sc_socket(uint32_t, uint32_t, uint32_t))
DECL(sc_socketpair(uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_bind(uint32_t, uint32_t, uint32_t))
DECL(sc_connect(uint32_t, uint32_t, uint32_t))
DECL(sc_listen(uint32_t, uint32_t))
DECL(sc_accept4(uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_getsockopt(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_setsockopt(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_getsockname(uint32_t, uint32_t, uint32_t))
DECL(sc_getpeername(uint32_t, uint32_t, uint32_t))
DECL(sc_sendto(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_sendmsg(uint32_t, uint32_t, uint32_t))
DECL(sc_recvfrom(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
DECL(sc_recvmsg(uint32_t, uint32_t, uint32_t))
DECL(sc_shutdown(uint32_t, uint32_t))

#endif
