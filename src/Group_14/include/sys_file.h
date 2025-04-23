/**
 * @file sys_file.h
 * @brief Kernel-Level File Operation Interface
 *
 * Defines structures and function prototypes for internal kernel functions
 * that implement the logic for file-related system calls (open, read, write, etc.).
 * These functions typically interact with the Virtual File System (VFS) layer.
 */

// Include Guard
#ifndef SYS_FILE_H
#define SYS_FILE_H

#pragma once // Optional but common

#include "types.h"      // For off_t, ssize_t, size_t (ensure size_t is robustly defined here or libc)
#include "vfs.h"        // Required for the definition of file_t

#ifdef __cplusplus
extern "C" {
#endif

// === File Open Flags ===
// These flags mirror standard POSIX definitions for compatibility and clarity.
// The access modes (O_RDONLY, O_WRONLY, O_RDWR) are mutually exclusive in the low bits.
#define O_ACCMODE   0x0003  /**< Mask for access modes */
#define O_RDONLY    0x0000  /**< Open for reading only */
#define O_WRONLY    0x0001  /**< Open for writing only */
#define O_RDWR      0x0002  /**< Open for reading and writing */
// Other status flags are OR'd in:
#define O_CREAT     0x0100  /**< Create file if it does not exist */
#define O_TRUNC     0x0200  /**< Truncate size to 0 upon opening */
#define O_APPEND    0x0400  /**< Seek to end of file before each write */
#define O_EXCL      0x0800  /**< Used with O_CREAT: fail if file exists */
// Add other flags like O_DIRECTORY, O_NOFOLLOW, O_NONBLOCK etc. if needed by VFS/drivers

// === Whence Values for lseek ===
// Standard POSIX values
#ifndef SEEK_SET // Define only if not already defined (e.g., by another system header)
#define SEEK_SET    0   /**< Seek relative to the beginning of the file. */
#define SEEK_CUR    1   /**< Seek relative to the current file position. */
#define SEEK_END    2   /**< Seek relative to the end of the file. */
#endif

// === Structures ===

/**
 * @brief Kernel's internal representation of an open file description.
 *
 * This structure links a file descriptor number (which is the index into the
 * process's fd_table) to an underlying VFS file object (`file_t`). It stores
 * the flags the file was opened with. The actual file position (offset) and
 * other file-specific state are typically managed within the `file_t` structure
 * provided by the VFS layer.
 *
 * Instances of this struct are usually allocated dynamically when a file is opened.
 */
typedef struct sys_file {
    file_t *vfs_file; /**< Pointer to the underlying VFS file structure. */
    int flags;        /**< Flags used when opening the file (O_RDONLY, O_APPEND, etc.). */
    // Note: File position (offset) is managed within vfs_file->f_pos
    // Note: Reference counting for fork/dup is not implemented here yet.
} sys_file_t;


// === Function Prototypes ===
// These functions are typically called by the syscall implementation layer.
// They operate within the context of the current process.

/**
 * @brief Kernel implementation for opening or creating a file.
 * Translates path, validates flags/mode, interacts with VFS to get a `file_t`,
 * allocates a `sys_file_t`, and assigns it to a free file descriptor
 * in the current process's `fd_table`.
 *
 * @param pathname Null-terminated path to the file (already copied to kernel space).
 * @param flags File open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.).
 * @param mode File mode (permissions) used only when O_CREAT is specified.
 * (Implementation might ignore mode on simple filesystems like FAT).
 * @return File descriptor (int >= 0) on success.
 * @return Negative errno (from fs_errno.h) on failure.
 */
int sys_open(const char *pathname, int flags, int mode);

/**
 * @brief Kernel implementation for reading from an open file descriptor.
 * Validates the FD, retrieves the corresponding `sys_file_t`, checks open flags,
 * and calls the VFS read operation.
 *
 * @param fd File descriptor number.
 * @param kbuf Kernel buffer where read data should be stored. Must be valid kernel memory.
 * @param count Maximum number of bytes to read into `kbuf`.
 * @return Number of bytes read (>= 0) on success (0 indicates End-Of-File).
 * @return Negative errno on failure (e.g., -EBADF if fd is invalid, -EIO for device error).
 */
ssize_t sys_read(int fd, void *kbuf, size_t count);

/**
 * @brief Kernel implementation for writing to an open file descriptor.
 * Validates the FD, retrieves the corresponding `sys_file_t`, checks open flags
 * (e.g., O_WRONLY/O_RDWR, O_APPEND), and calls the VFS write operation.
 *
 * @param fd File descriptor number.
 * @param kbuf Kernel buffer containing the data to write. Must be valid kernel memory.
 * @param count Number of bytes to write from `kbuf`.
 * @return Number of bytes written (>= 0) on success. May be less than `count` if disk is full.
 * @return Negative errno on failure (e.g., -EBADF, -ENOSPC, -EIO).
 */
ssize_t sys_write(int fd, const void *kbuf, size_t count);

/**
 * @brief Kernel implementation for closing an open file descriptor.
 * Validates the FD, retrieves the `sys_file_t`, calls the VFS close operation
 * on the underlying `file_t`, frees the `sys_file_t` structure, and marks
 * the FD slot in the process's `fd_table` as available.
 *
 * @param fd File descriptor to close.
 * @return 0 on success.
 * @return Negative errno on failure (e.g., -EBADF).
 */
int sys_close(int fd);

/**
 * @brief Kernel implementation for repositioning the file offset.
 * Validates the FD, retrieves the `sys_file_t`, validates `whence`, and calls
 * the VFS lseek operation on the underlying `file_t`.
 *
 * @param fd File descriptor number.
 * @param offset Offset value (can be negative for SEEK_CUR/SEEK_END).
 * @param whence Directive for positioning: SEEK_SET, SEEK_CUR, or SEEK_END.
 * @return The resulting offset location from the beginning of the file (>= 0) on success.
 * @return Negative errno on failure (e.g., -EBADF, -EINVAL, -ESPIPE for non-seekable files).
 */
off_t sys_lseek(int fd, off_t offset, int whence);


#ifdef __cplusplus
}
#endif

#endif /* SYS_FILE_H */