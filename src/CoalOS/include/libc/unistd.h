#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include "stddef.h"
#include "stdint.h"
#include "stdarg.h"

// Basic types
typedef int32_t ssize_t;
typedef int32_t off_t;
typedef int32_t pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t mode_t;
typedef uint32_t useconds_t;
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;

// File descriptor constants
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Open flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800

// Seek constants
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Access mode constants
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

// File operations
int open(const char *path, int flags, ...);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);

// Process operations
pid_t getpid(void);
pid_t getppid(void);
void exit(int status) __attribute__((noreturn));
pid_t fork(void);
int execve(const char *filename, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);

// Directory operations
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int mkdir(const char *pathname, mode_t mode);
int rmdir(const char *pathname);
int unlink(const char *pathname);

// User/Group IDs
uid_t getuid(void);
gid_t getgid(void);
uid_t geteuid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);

// Memory management
void *sbrk(intptr_t increment);
int brk(void *addr);

// File descriptor operations
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);

// Sleep functions
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);

// File access
int access(const char *pathname, int mode);

// Process control
int pause(void);

// Synchronization
int sync(void);

#endif // LIBC_UNISTD_H