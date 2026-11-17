#ifndef LIBC_SYS_STATFS_H
#define LIBC_SYS_STATFS_H

/* Linux x86_64 struct statfs (120 bytes). */
struct statfs {
    long f_type, f_bsize;
    unsigned long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int f_fsid[2];
    long f_namelen, f_frsize, f_flags, f_spare[4];
};

int statfs(const char *path, struct statfs *buf);

#endif
