// Include Guard
#ifndef SYS_FILE_H
#define SYS_FILE_H

#pragma once // Optional but common

#include "types.h"      // For off_t, ssize_t
#include <libc/stddef.h> // For size_t definition robustness
#include "vfs.h"        // <<<--- Include VFS header for file_t definition

#ifdef __cplusplus
extern "C" {
#endif

/* File open flags (bitmask values - POSIX like) */
#define O_RDONLY    0x0000  /* Open for reading only */
#define O_WRONLY    0x0001  /* Open for writing only */
#define O_RDWR      0x0002  /* Open for reading and writing */
// Access mode mask: O_ACCMODE 0x0003
#define O_CREAT     0x0100  /* Create file if it does not exist */
#define O_TRUNC     0x0200  /* Truncate size to 0 */
#define O_APPEND    0x0400  /* Append on each write */
// Add other flags like O_EXCL, O_DIRECTORY, O_NOFOLLOW etc. if needed

/* Whence values for lseek */
#ifndef SEEK_SET // Define only if not already defined (e.g., by another header)
#define SEEK_SET    0   /* Seek from beginning of file.  */
#define SEEK_CUR    1   /* Seek from current position.  */
#define SEEK_END    2   /* Seek from end of file.  */
#endif

// === Structure Definition ===

/**
 * @brief sys_file_t
 *
 * Represents an open file description within the kernel for a specific process.
 * This structure is allocated dynamically and pointed to by the process's fd_table.
 */
typedef struct sys_file {
    // int fd;             // FD number is the *index* in the process table, not stored here
    file_t *vfs_file;       // Underlying file structure from the VFS layer
    int flags;              // Open flags (e.g., O_RDONLY, O_WRONLY, O_RDWR, O_APPEND) - inherited from VFS/open
    // off_t pos;           // Position is managed within the underlying VFS file_t structure
    // int ref_count;       // Basic implementation: no dup/fork support yet, so ref_count is implicitly 1
} sys_file_t;


// === Function Prototypes ===

/**
 * @brief sys_open - Opens or creates a file. Corresponds to the open() syscall.
 * @pathname: Null-terminated path to the file (in kernel space).
 * @flags: File open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.).
 * @mode: File mode (permissions) to use when creating a file (often ignored in simple FS).
 *
 * @return File descriptor (>= 0) on success, negative errno on failure.
 */
int sys_open(const char *pathname, int flags, int mode);

/**
 * @brief sys_read - Reads data from an open file descriptor. Corresponds to read() syscall.
 * @fd: File descriptor.
 * @kbuf: Kernel buffer to store read data.
 * @count: Maximum number of bytes to read.
 *
 * @return Number of bytes read (>= 0) on success (0 indicates EOF), negative errno on failure.
 */
ssize_t sys_read(int fd, void *kbuf, size_t count);

/**
 * @brief sys_write - Writes data to an open file descriptor. Corresponds to write() syscall.
 * @fd: File descriptor.
 * @kbuf: Kernel buffer containing data to write.
 * @count: Number of bytes to write.
 *
 * @return Number of bytes written (>= 0) on success, negative errno on failure.
 */
ssize_t sys_write(int fd, const void *kbuf, size_t count);

/**
 * @brief sys_close - Closes an open file descriptor. Corresponds to close() syscall.
 * @fd: File descriptor to close.
 *
 * @return 0 on success, negative errno on failure.
 */
int sys_close(int fd);

/**
 * @brief sys_lseek - Repositions the file offset associated with a file descriptor. Corresponds to lseek() syscall.
 * @fd: File descriptor.
 * @offset: Offset value.
 * @whence: One of SEEK_SET, SEEK_CUR, or SEEK_END.
 *
 * @return The resulting offset location from the beginning of the file on success,
 * negative errno on failure.
 */
off_t sys_lseek(int fd, off_t offset, int whence);


#ifdef __cplusplus
}
#endif

#endif /* SYS_FILE_H */
