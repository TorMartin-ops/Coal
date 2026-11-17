/* System call wrappers. Failures return -1 and set errno (Linux i386 ABI). */

#include <libc/syscall.h>
#include <libc/errno.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/signal.h>
#include <libc/termios.h>
#include <libc/time.h>
#include <libc/dirent.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/sys/stat.h>
#include <libc/sys/wait.h>
#include <libc/sys/utsname.h>
#include <libc/sys/select.h>
#include <libc/sys/ioctl.h>
#include <libc/sys/statfs.h>
#include <libc/sys/socket.h>

int errno;
char **environ;

long __syscall_ret(long r) {
    if ((unsigned long)r > (unsigned long)-4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define RET(x) return (int)__syscall_ret(x)

ssize_t read(int fd, void *b, size_t n)        { RET(__SC3(SYS_READ, fd, b, n)); }
ssize_t write(int fd, const void *b, size_t n) { RET(__SC3(SYS_WRITE, fd, b, n)); }
int close(int fd)                              { RET(__SC1(SYS_CLOSE, fd)); }
off_t lseek(int fd, off_t o, int w)            { RET(__SC3(SYS_LSEEK, fd, o, w)); }
int dup(int fd)                                { RET(__SC1(SYS_DUP, fd)); }
int dup2(int a, int b)                         { RET(__SC3(SYS_DUP2, a, b, 0)); }
int pipe(int fds[2])                           { RET(__SC1(SYS_PIPE, fds)); }
int unlink(const char *p)                      { RET(__SC1(SYS_UNLINK, p)); }
int rmdir(const char *p)                       { RET(__SC1(SYS_RMDIR, p)); }
int access(const char *p, int m)               { RET(__SC3(SYS_ACCESS, p, m, 0)); }
int chdir(const char *p)                       { RET(__SC1(SYS_CHDIR, p)); }
int rename(const char *a, const char *b)       { RET(__SC3(SYS_RENAME, a, b, 0)); }
int mkdir(const char *p, mode_t m)             { RET(__SC3(SYS_MKDIR, p, m, 0)); }
void sync(void)                                { __SC1(SYS_SYNC, 0); }
int fsync(int fd)                              { RET(__SC1(SYS_FSYNC, fd)); }
int link(const char *a, const char *b)         { RET(__SC3(SYS_LINK, a, b, 0)); }
int symlink(const char *a, const char *b)      { RET(__SC3(SYS_SYMLINK, a, b, 0)); }
ssize_t readlink(const char *p, char *b, size_t n) { RET(__SC3(SYS_READLINK, p, b, n)); }
int chmod(const char *p, mode_t m)             { RET(__SC3(SYS_CHMOD, p, m, 0)); }
int statfs(const char *p, struct statfs *s)    { RET(__SC3(SYS_STATFS, p, s, 0)); }
int gettimeofday(struct timeval *tv, void *tz) { RET(__SC3(SYS_GETTIMEOFDAY, tv, tz, 0)); }
int socketpair(int d, int t, int p, int fds[2]) { RET(__sc(SYS_SOCKETPAIR, d, t, p, (long)fds, 0)); }

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
    RET(__sc(SYS_NEWSELECT, n, (long)r, (long)w, (long)e, (long)tv));
}

int ioctl(int fd, unsigned long req, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, req);
    void *arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);
    RET(__SC3(SYS_IOCTL, fd, req, arg));
}

int open(const char *path, int flags, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? __builtin_va_arg(ap, mode_t) : 0;
    __builtin_va_end(ap);
    RET(__SC3(SYS_OPEN, path, flags, mode));
}

int fcntl(int fd, int cmd, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    long arg = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);
    RET(__SC3(SYS_FCNTL64, fd, cmd, arg));
}

char *getcwd(char *buf, size_t n) {
    return __syscall_ret(__SC3(SYS_GETCWD, buf, n, 0)) < 0 ? NULL : buf;
}

int stat(const char *p, struct stat *s)   { RET(__SC3(SYS_STAT64, p, s, 0)); }
int lstat(const char *p, struct stat *s)  { RET(__SC3(SYS_LSTAT64, p, s, 0)); }
int fstat(int fd, struct stat *s)         { RET(__SC3(SYS_FSTAT64, fd, s, 0)); }
int uname(struct utsname *u)              { RET(__SC1(SYS_UNAME, u)); }

pid_t fork(void)                          { RET(__SC1(SYS_FORK, 0)); }
pid_t getpid(void)                        { return (pid_t)__SC1(SYS_GETPID, 0); }
pid_t getppid(void)                       { return (pid_t)__SC1(SYS_GETPPID, 0); }
pid_t setsid(void)                        { RET(__SC1(SYS_SETSID, 0)); }
int setpgid(pid_t p, pid_t g)             { RET(__SC3(SYS_SETPGID, p, g, 0)); }
uid_t getuid(void)                        { return (uid_t)__SC1(SYS_GETUID, 0); }
int kill(pid_t p, int s)                  { RET(__SC3(SYS_KILL, p, s, 0)); }
time_t time(time_t *t)                    { RET(__SC1(SYS_TIME, t)); }
int nanosleep(const struct timespec *r, struct timespec *m) { RET(__SC3(SYS_NANOSLEEP, r, m, 0)); }

/* Adds inc to the nice value and returns the new one; the kernel's getpriority answers 20 - nice. */
int nice(int inc) {
    long r = __SC1(SYS_NICE, inc);
    if (r < 0) { errno = (int)-r; return -1; }
    return 20 - (int)__SC3(SYS_GETPRIORITY, 0, 0, 0);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    RET(__sc(SYS_WAIT4, pid, (long)status, options, 0, 0));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    RET(__SC3(SYS_EXECVE, path, argv, envp));
}

int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }

/* Searches PATH like execvp(3); a name with a slash is used as is. */
int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/')) return execv(file, argv);
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    size_t flen = strlen(file);
    int err = ENOENT;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t dlen = end ? (size_t)(end - path) : strlen(path);
        char buf[256];
        if (dlen + flen + 2 <= sizeof buf) {
            memcpy(buf, path, dlen);
            buf[dlen] = '/';
            strcpy(buf + dlen + 1, file);
            execv(buf, argv);
            if (errno != ENOENT) err = errno;
        }
        path += dlen + (end != NULL);
    }
    errno = err;
    return -1;
}

void _exit(int status) {
    for (;;) __SC1(SYS_EXIT, status);
}

int isatty(int fd) {
    struct termios t;
    return tcgetattr(fd, &t) == 0;
}

unsigned sleep(unsigned sec) {
    struct timespec ts = { (long)sec, 0 };
    return nanosleep(&ts, &ts) ? (unsigned)ts.tv_sec : 0;
}

int usleep(unsigned usec) {
    struct timespec ts = { (long)(usec / 1000000), (long)(usec % 1000000) * 1000 };
    return nanosleep(&ts, NULL);
}

/* ioctl numbers shared with the kernel's tty driver */
#define TCGETS 0x5401
#define TCSETS 0x5402

int tcgetattr(int fd, struct termios *t) { RET(__SC3(SYS_IOCTL, fd, TCGETS, t)); }
int tcsetattr(int fd, int when, const struct termios *t) {
    RET(__SC3(SYS_IOCTL, fd, TCSETS + when, t));
}

/* rt_sigaction needs a restorer that calls rt_sigreturn. */
__asm__(".text\n__sigreturn:\n\tmovl $173, %eax\n\tint $0x80\n");
extern void __sigreturn(void);

struct ksigaction {
    sighandler_t handler;
    unsigned long flags;
    void (*restorer)(void);
    unsigned long long mask;
};

sighandler_t signal(int sig, sighandler_t h) {
    struct ksigaction sa = { h, 0x04000004 /* SA_RESTORER | SA_SIGINFO: rt frame, ends in rt_sigreturn */, __sigreturn, 0 }, old;
    return __syscall_ret(__sc(SYS_RT_SIGACTION, sig, (long)&sa, (long)&old, 8, 0)) < 0
               ? (sighandler_t)-1 : old.handler;
}

struct DIR {
    int fd, pos, len;
    char buf[512];
};

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof *d);
    if (!d) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (d->pos >= d->len) {
        long n = __syscall_ret(__SC3(SYS_GETDENTS64, d->fd, d->buf, sizeof d->buf));
        if (n <= 0) return NULL;
        d->pos = 0;
        d->len = (int)n;
    }
    struct dirent *e = (struct dirent *)(d->buf + d->pos);
    d->pos += e->d_reclen;
    return e;
}

int closedir(DIR *d) {
    int r = close(d->fd);
    free(d);
    return r;
}
