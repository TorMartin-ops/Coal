#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include <libc/sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

extern char **environ;
extern char *optarg;
extern int optind, opterr, optopt;

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     dup(int fd);
int     dup2(int from, int to);
int     pipe(int fds[2]);
int     unlink(const char *path);
int     rmdir(const char *path);
int     access(const char *path, int mode);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t n);
int     isatty(int fd);
unsigned sleep(unsigned sec);
int     usleep(unsigned usec);
int     nice(int inc);
int     fsync(int fd);
int     link(const char *from, const char *to);
int     symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t n);
int     chmod(const char *path, mode_t mode);
void    sync(void);

pid_t   fork(void);
pid_t   getpid(void);
pid_t   getppid(void);
pid_t   setsid(void);
int     setpgid(pid_t pid, pid_t pgid);
uid_t   getuid(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
void    _exit(int status) __attribute__((noreturn));

int     getopt(int argc, char *const argv[], const char *opts);

#endif
