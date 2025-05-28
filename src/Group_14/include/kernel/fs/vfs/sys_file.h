/* include/sys_file.h - Using Standard POSIX-like Flag Values */
#ifndef SYS_FILE_H
#define SYS_FILE_H

#pragma once

#include <kernel/core/types.h>
#include <kernel/fs/vfs/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

// === File Open Flags (POSIX Values) ===
#define O_ACCMODE   0x0003  // Mask for access modes (remains same)
#define O_RDONLY    0x0000  // Read only
#define O_WRONLY    0x0001  // Write only
#define O_RDWR      0x0002  // Read/write

// File creation flags (use standard values)
#define O_CREAT     0x0040  // Create file if it does not exist (Bit 6)
#define O_EXCL      0x0080  // Exclusive use flag (Bit 7)
#define O_NOCTTY    0x0100  // Do not assign controlling terminal
#define O_TRUNC     0x0200  // Truncate flag (Bit 9)

// File status flags
#define O_APPEND    0x0400  // Set append mode (Bit 10)
// Add O_NONBLOCK, O_SYNC, O_DSYNC, O_DIRECTORY, O_NOFOLLOW etc. as needed

// === Whence Values for lseek === (Unchanged)
#ifndef SEEK_SET
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2
#endif

// === Structures === (Unchanged)
typedef struct sys_file {
    file_t *vfs_file;
    int flags;
} sys_file_t;


// === Function Prototypes === (Unchanged)
int sys_open(const char *pathname, int flags, int mode);
ssize_t sys_read(int fd, void *kbuf, size_t count);
ssize_t sys_write(int fd, const void *kbuf, size_t count);
int sys_close(int fd);
off_t sys_lseek(int fd, off_t offset, int whence);


#ifdef __cplusplus
}
#endif

#endif /* SYS_FILE_H */