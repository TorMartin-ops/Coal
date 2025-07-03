/**
 * @file unistd.c
 * @brief POSIX system call wrappers for Coal OS userspace
 */

#include <libc/unistd.h>
#include <libc/stddef.h>

// System call numbers (must match kernel definitions)
#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_CREAT   8
#define SYS_LINK    9
#define SYS_UNLINK  10
#define SYS_EXECVE  11
#define SYS_CHDIR   12
#define SYS_TIME    13
#define SYS_MKNOD   14
#define SYS_CHMOD   15
#define SYS_LCHOWN  16
#define SYS_STAT    18
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_MOUNT   21
#define SYS_UMOUNT  22
#define SYS_SETUID  23
#define SYS_GETUID  24
#define SYS_STIME   25
#define SYS_PTRACE  26
#define SYS_ALARM   27
#define SYS_FSTAT   28
#define SYS_PAUSE   29
#define SYS_UTIME   30
#define SYS_ACCESS  33
#define SYS_NICE    34
#define SYS_SYNC    36
#define SYS_KILL    37
#define SYS_RENAME  38
#define SYS_MKDIR   39
#define SYS_RMDIR   40
#define SYS_DUP     41
#define SYS_PIPE    42
#define SYS_TIMES   43
#define SYS_PROF    44
#define SYS_BRK     45
#define SYS_SETGID  46
#define SYS_GETGID  47
#define SYS_SIGNAL  48
#define SYS_GETEUID 49
#define SYS_GETEGID 50

// System call wrapper function
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int result;
    __asm__ volatile (
        "pushl %%ebx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "movl %1, %%eax\n\t"
        "movl %2, %%ebx\n\t"
        "movl %3, %%ecx\n\t"
        "movl %4, %%edx\n\t"
        "int $0x80\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%ebx\n\t"
        : "=a" (result)
        : "m" (num), "m" (arg1), "m" (arg2), "m" (arg3)
        : "cc", "memory"
    );
    return result;
}

// File operations
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        // Extract mode from variadic arguments
        __builtin_va_list args;
        __builtin_va_start(args, flags);
        mode = __builtin_va_arg(args, mode_t);
        __builtin_va_end(args);
    }
    return syscall(SYS_OPEN, (int)(uintptr_t)path, flags, mode);
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall(SYS_READ, fd, (int)(uintptr_t)buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall(SYS_WRITE, fd, (int)(uintptr_t)buf, count);
}

int close(int fd) {
    return syscall(SYS_CLOSE, fd, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall(SYS_LSEEK, fd, offset, whence);
}

// Process operations
pid_t getpid(void) {
    return (pid_t)syscall(SYS_GETPID, 0, 0, 0);
}

pid_t getppid(void) {
    // For now, use getpid - we don't have getppid syscall yet
    return (pid_t)syscall(SYS_GETPID, 0, 0, 0);
}

void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
    // Should not return
    while(1);
}

pid_t fork(void) {
    return (pid_t)syscall(SYS_FORK, 0, 0, 0);
}

int execve(const char *filename, char *const argv[], char *const envp[]) {
    return syscall(SYS_EXECVE, (int)(uintptr_t)filename, 
                   (int)(uintptr_t)argv, (int)(uintptr_t)envp);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)syscall(SYS_WAITPID, pid, (int)(uintptr_t)status, options);
}

// Directory operations
int chdir(const char *path) {
    return syscall(SYS_CHDIR, (int)(uintptr_t)path, 0, 0);
}

char *getcwd(char *buf, size_t size) {
    // This would need a proper getcwd syscall
    // For now, return a placeholder
    if (buf && size > 1) {
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }
    return NULL;
}

int mkdir(const char *pathname, mode_t mode) {
    return syscall(SYS_MKDIR, (int)(uintptr_t)pathname, mode, 0);
}

int rmdir(const char *pathname) {
    return syscall(SYS_RMDIR, (int)(uintptr_t)pathname, 0, 0);
}

int unlink(const char *pathname) {
    return syscall(SYS_UNLINK, (int)(uintptr_t)pathname, 0, 0);
}

// User/Group IDs
uid_t getuid(void) {
    return (uid_t)syscall(SYS_GETUID, 0, 0, 0);
}

gid_t getgid(void) {
    return (gid_t)syscall(SYS_GETGID, 0, 0, 0);
}

uid_t geteuid(void) {
    return (uid_t)syscall(SYS_GETEUID, 0, 0, 0);
}

gid_t getegid(void) {
    return (gid_t)syscall(SYS_GETEGID, 0, 0, 0);
}

int setuid(uid_t uid) {
    return syscall(SYS_SETUID, uid, 0, 0);
}

int setgid(gid_t gid) {
    return syscall(SYS_SETGID, gid, 0, 0);
}

// Memory management
void *sbrk(intptr_t increment) {
    static void *current_brk = NULL;
    
    if (current_brk == NULL) {
        // Get current break
        current_brk = (void*)syscall(SYS_BRK, 0, 0, 0);
        if (current_brk == (void*)-1) {
            return (void*)-1;
        }
    }
    
    if (increment == 0) {
        return current_brk;
    }
    
    void *old_brk = current_brk;
    void *new_brk = (void*)((char*)current_brk + increment);
    
    void *result = (void*)syscall(SYS_BRK, (int)(uintptr_t)new_brk, 0, 0);
    if (result == (void*)-1) {
        return (void*)-1;
    }
    
    current_brk = new_brk;
    return old_brk;
}

int brk(void *addr) {
    void *result = (void*)syscall(SYS_BRK, (int)(uintptr_t)addr, 0, 0);
    return (result == (void*)-1) ? -1 : 0;
}

// File descriptor operations
int dup(int oldfd) {
    return syscall(SYS_DUP, oldfd, 0, 0);
}

int dup2(int oldfd, int newfd) {
    // For now, just use dup - we don't have dup2 syscall yet
    return syscall(SYS_DUP, oldfd, newfd, 0);
}

int pipe(int pipefd[2]) {
    return syscall(SYS_PIPE, (int)(uintptr_t)pipefd, 0, 0);
}

// Sleep functions
unsigned int sleep(unsigned int seconds) {
    // This would need a proper sleep syscall or nanosleep
    // For now, busy wait (very inefficient)
    for (unsigned int i = 0; i < seconds * 1000000; i++) {
        __asm__ volatile ("nop");
    }
    return 0;
}

int usleep(useconds_t usec) {
    // Busy wait for microseconds
    for (useconds_t i = 0; i < usec; i++) {
        __asm__ volatile ("nop");
    }
    return 0;
}

// File access
int access(const char *pathname, int mode) {
    return syscall(SYS_ACCESS, (int)(uintptr_t)pathname, mode, 0);
}

// Process control
int pause(void) {
    return syscall(SYS_PAUSE, 0, 0, 0);
}

// Synchronization
int sync(void) {
    return syscall(SYS_SYNC, 0, 0, 0);
}