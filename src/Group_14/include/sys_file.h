#pragma once
#ifndef SYS_FILE_H
#define SYS_FILE_H

#include "types.h"  // For off_t, ssize_t

#ifdef __cplusplus
extern "C" {
#endif

/* File open flags (bitmask values) */
#define O_RDONLY    0x0000  /* Open for reading only */
#define O_WRONLY    0x0001  /* Open for writing only */
#define O_RDWR      0x0002  /* Open for reading and writing */
#define O_CREAT     0x0100  /* Create file if it does not exist */
#define O_TRUNC     0x0200  /* Truncate file upon open */
#define O_APPEND    0x0400  /* Append on each write */

/* Whence values for lseek */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/**
 * sys_open - Opens a file for use by the process.
 * @pathname: Null‑terminated path to the file.
 * @flags: File open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.).
 * @mode: File mode (permissions) to use when creating a file.
 *
 * Returns a non‑negative file descriptor on success, or -1 on error.
 */
int sys_open(const char *pathname, int flags, int mode);

/**
 * sys_read - Reads data from an open file descriptor.
 * @fd: File descriptor.
 * @buf: Buffer to store data.
 * @count: Number of bytes to read.
 *
 * Returns the number of bytes read, or -1 on error.
 */
ssize_t sys_read(int fd, void *buf, size_t count);

/**
 * sys_write - Writes data to an open file descriptor.
 * @fd: File descriptor.
 * @buf: Buffer containing data to write.
 * @count: Number of bytes to write.
 *
 * Returns the number of bytes written, or -1 on error.
 */
ssize_t sys_write(int fd, const void *buf, size_t count);

/**
 * sys_close - Closes an open file descriptor.
 * @fd: File descriptor to close.
 *
 * Returns 0 on success, or -1 on error.
 */
int sys_close(int fd);

/**
 * sys_lseek - Repositions the file offset associated with a file descriptor.
 * @fd: File descriptor.
 * @offset: Offset to set.
 * @whence: One of SEEK_SET, SEEK_CUR, or SEEK_END.
 *
 * Returns the new offset on success, or -1 on error.
 */
off_t sys_lseek(int fd, off_t offset, int whence);

#ifdef __cplusplus
}
#endif

#endif /* SYS_FILE_H */
