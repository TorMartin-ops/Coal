/**
 * @file unistd.c
 * @brief Thin wrappers around the system calls. Results are returned as the
 *        kernel gives them: a negative value is a negated error number.
 */

#include <libc/unistd.h>
#include <libc/signal.h>
#include <libc/syscall.h>

/* A raw return value in the last 4095 values below 2^32 is an error. */
static int is_error_ptr(int value) {
    return (unsigned int)value >= (unsigned int)-4095;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list args;
        __builtin_va_start(args, flags);
        mode = __builtin_va_arg(args, mode_t);
        __builtin_va_end(args);
    }
    return syscall(SYS_OPEN, (int)(uintptr_t)path, flags, (int)mode);
}

ssize_t read(int fd, void *buf, size_t count) {
    return syscall(SYS_READ, fd, (int)(uintptr_t)buf, (int)count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return syscall(SYS_WRITE, fd, (int)(uintptr_t)buf, (int)count);
}

int close(int fd) {
    return syscall(SYS_CLOSE, fd, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    return syscall(SYS_LSEEK, fd, (int)offset, whence);
}

pid_t getpid(void) {
    return syscall(SYS_GETPID, 0, 0, 0);
}

pid_t getppid(void) {
    return syscall(SYS_GETPPID, 0, 0, 0);
}

void _exit(int status) {
    /* exit() in stdlib.c runs the atexit handlers and then calls this. */
    syscall(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

pid_t fork(void) {
    return syscall(SYS_FORK, 0, 0, 0);
}

int execve(const char *filename, char *const argv[], char *const envp[]) {
    return syscall(SYS_EXECVE, (int)(uintptr_t)filename,
                   (int)(uintptr_t)argv, (int)(uintptr_t)envp);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return syscall(SYS_WAITPID, pid, (int)(uintptr_t)status, options);
}

int kill(pid_t pid, int sig) {
    return syscall(SYS_KILL, pid, sig, 0);
}

int chdir(const char *path) {
    return syscall(SYS_CHDIR, (int)(uintptr_t)path, 0, 0);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return NULL;
    if (syscall(SYS_GETCWD, (int)(uintptr_t)buf, (int)size, 0) < 0) return NULL;
    return buf;
}

int mkdir(const char *pathname, mode_t mode) {
    return syscall(SYS_MKDIR, (int)(uintptr_t)pathname, (int)mode, 0);
}

int rmdir(const char *pathname) {
    return syscall(SYS_RMDIR, (int)(uintptr_t)pathname, 0, 0);
}

int unlink(const char *pathname) {
    return syscall(SYS_UNLINK, (int)(uintptr_t)pathname, 0, 0);
}

/* Linux brk(): returns the new break on success, an error value on failure. */
void *sbrk(intptr_t increment) {
    static char *current_brk = NULL;

    if (!current_brk) {
        int r = syscall(SYS_BRK, 0, 0, 0);
        if (is_error_ptr(r)) return (void *)-1;
        current_brk = (char *)(uintptr_t)r;
    }
    if (increment == 0) return current_brk;

    char *new_brk = current_brk + increment;
    int r = syscall(SYS_BRK, (int)(uintptr_t)new_brk, 0, 0);
    if (is_error_ptr(r)) return (void *)-1;

    char *old_brk = current_brk;
    current_brk = new_brk;
    return old_brk;
}

int brk(void *addr) {
    int r = syscall(SYS_BRK, (int)(uintptr_t)addr, 0, 0);
    return is_error_ptr(r) ? -1 : 0;
}

int dup2(int oldfd, int newfd) {
    return syscall(SYS_DUP2, oldfd, newfd, 0);
}

int pipe(int pipefd[2]) {
    return syscall(SYS_PIPE, (int)(uintptr_t)pipefd, 0, 0);
}
