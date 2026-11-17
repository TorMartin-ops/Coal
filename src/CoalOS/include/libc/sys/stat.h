#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#include <libc/sys/types.h>

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100

/* Linux i386 struct stat64 (96 bytes). */
struct stat {
    unsigned long long st_dev;
    unsigned int       __pad0;
    unsigned int       __ino32;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;
    unsigned int       __pad3;
    long long          st_size;
    unsigned int       st_blksize;
    unsigned long long st_blocks;
    long               st_atime, st_atime_nsec;
    long               st_mtime, st_mtime_nsec;
    long               st_ctime, st_ctime_nsec;
    unsigned long long st_ino;
};

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);

#endif
