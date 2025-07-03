/**
 * @file dirent.h
 * @brief Directory entry structures and definitions
 * @author Coal OS Kernel Team
 * @version 1.0
 */

#ifndef DIRENT_H
#define DIRENT_H

#include <libc/stdint.h>
#include <kernel/core/types.h>

// Directory entry types (for d_type field)
#ifndef DT_UNKNOWN
#define DT_UNKNOWN  0   // Unknown type
#define DT_FIFO     1   // FIFO (named pipe)
#define DT_CHR      2   // Character device
#define DT_DIR      4   // Directory
#define DT_BLK      6   // Block device
#define DT_REG      8   // Regular file
#define DT_LNK      10  // Symbolic link
#define DT_SOCK     12  // Socket
#define DT_WHT      14  // Whiteout
#endif

// Maximum filename length
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

// struct dirent is already defined in types.h

/**
 * @brief Linux getdents system call directory entry structure
 * 
 * This is the structure used by the Linux getdents() system call,
 * which is different from the POSIX readdir() structure.
 */
struct linux_dirent {
    unsigned long  d_ino;       // Inode number
    unsigned long  d_off;       // Offset to next linux_dirent
    unsigned short d_reclen;    // Length of this record
    char           d_name[];    // Filename (null-terminated)
    /* After d_name, there's a null byte, then:
     * char d_type;              // File type (at d_reclen - 1)
     */
};

/**
 * @brief Calculate the size of a linux_dirent structure
 * @param namelen Length of the filename (not including null terminator)
 * @return Size of the structure in bytes
 */
static inline size_t linux_dirent_size(size_t namelen) {
    // Base size + name + null terminator + type byte
    // Aligned to 8-byte boundary
    size_t size = sizeof(struct linux_dirent) + namelen + 1 + 1;
    return (size + 7) & ~7;  // Round up to 8-byte boundary
}

/**
 * @brief Get the d_type field from a linux_dirent
 * @param d Pointer to linux_dirent structure
 * @return File type
 */
static inline unsigned char linux_dirent_type(struct linux_dirent *d) {
    return *((unsigned char *)d + d->d_reclen - 1);
}

/**
 * @brief Set the d_type field in a linux_dirent
 * @param d Pointer to linux_dirent structure
 * @param type File type to set
 */
static inline void linux_dirent_set_type(struct linux_dirent *d, unsigned char type) {
    *((unsigned char *)d + d->d_reclen - 1) = type;
}

#endif // DIRENT_H