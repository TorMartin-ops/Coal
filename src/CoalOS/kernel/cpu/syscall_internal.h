/* Private to the system call layer: helpers shared by syscall_*.c. */
#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include <kernel/cpu/syscall.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/memory/uaccess.h>
#include <kernel/process/process.h>

#define ARG_MAX_BYTES   32768       /* total size of argv + envp strings */
#define ARG_MAX_VECS    256         /* argv + envp entries incl. terminators */

/* Copy a NUL-terminated path from user space. 0, -EFAULT, -ENAMETOOLONG or -ENOENT (empty). */
int copy_path_from_user(const char *user, char *kbuf, size_t size);

/* Positive errno for a negative FS_ERR_* value; and the -errno form for return values. */
int fs_err_to_errno(int fs_err);
static inline long fs_ret(long r) { return r < 0 ? -(long)fs_err_to_errno((int)-r) : r; }

/* argv/envp copied once into kernel memory; free with exec_args_free. */
struct exec_args {
    char  *block;
    char **argv;
    char **envp;
};
long exec_args_copy(uint32_t user_argv, uint32_t user_envp, struct exec_args *out);
void exec_args_free(struct exec_args *a);

/* Open-file table of the calling process. */
sys_file_t *fd_get(int fd);                         /* referenced; release with sys_file_put */

long sc_read(uint32_t fd, uint32_t buf, uint32_t count);

/* Handlers outside syscall.c; the table there refers to them. */
long sc_write(uint32_t fd, uint32_t buf, uint32_t count);
long sc_open(uint32_t path, uint32_t flags, uint32_t mode);
long sc_openat(int dirfd, uint32_t path, uint32_t flags, uint32_t mode);
long sc_close(uint32_t fd);
long sc_lseek(uint32_t fd, int32_t off, uint32_t whence);
long sc_llseek(uint32_t fd, uint32_t hi, uint32_t lo, uint32_t result, uint32_t whence);
long sc_readv(uint32_t fd, uint32_t iov, uint32_t cnt);
long sc_writev(uint32_t fd, uint32_t iov, uint32_t cnt);
long sc_dup(uint32_t fd);
long sc_dup2(uint32_t oldfd, uint32_t newfd);
long sc_pipe(uint32_t fds);
long sc_pipe2(uint32_t fds, uint32_t flags);
long sc_ioctl(uint32_t fd, uint32_t cmd, uint32_t arg);
long sc_access(uint32_t path, uint32_t mode);
long sc_mkdir(uint32_t path, uint32_t mode);
long sc_rmdir(uint32_t path);
long sc_unlink(uint32_t path);
long sc_chdir(uint32_t path);
long sc_getcwd(uint32_t buf, uint32_t size);
long sc_stat64(uint32_t path, uint32_t buf);
long sc_fstat64(uint32_t fd, uint32_t buf);
long sc_fstatat64(int dirfd, uint32_t path, uint32_t buf, uint32_t flags);
long sc_getdents64(uint32_t fd, uint32_t buf, uint32_t count);
long sc_read_terminal_line(uint32_t buf, uint32_t count);

long sc_exit(uint32_t code);
long sc_fork(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, isr_frame_t *regs);
long sc_execve(uint32_t path, uint32_t argv, uint32_t envp, uint32_t d, uint32_t e, uint32_t f, isr_frame_t *regs);
long sc_wait4(int32_t pid, uint32_t status, uint32_t options, uint32_t rusage);
long sc_waitpid(int32_t pid, uint32_t status, uint32_t options);
long sc_getpid(void);
long sc_getppid(void);
long sc_gettid(void);
long sc_kill(int32_t pid, uint32_t sig);
long sc_setsid(void);
long sc_getsid(uint32_t pid);
long sc_setpgid(uint32_t pid, uint32_t pgid);
long sc_getpgid(uint32_t pid);
long sc_getpgrp(void);
long sc_sched_yield(void);

long sc_brk(uint32_t addr);
long sc_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t pgoff);
long sc_munmap(uint32_t addr, uint32_t len);

long sc_time(uint32_t tloc);
long sc_gettimeofday(uint32_t tv, uint32_t tz);
long sc_clock_gettime(uint32_t clk, uint32_t ts);
long sc_nanosleep(uint32_t req, uint32_t rem);
long sc_uname(uint32_t buf);
long sc_zero(void);                    /* getuid & co: always root */
long sc_set_thread_area(uint32_t desc);
long sc_set_tid_address(uint32_t ptr);
long sc_rt_sigaction(uint32_t sig, uint32_t act, uint32_t oact, uint32_t size);
long sc_rt_sigprocmask(uint32_t how, uint32_t set, uint32_t oset, uint32_t size);

#endif
