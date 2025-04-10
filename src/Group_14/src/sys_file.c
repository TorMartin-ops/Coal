#include "sys_file.h"
#include "vfs.h"         // Provides vfs_open, vfs_read, vfs_write, vfs_close, vfs_lseek
#include "terminal.h"    // For diagnostic output
#include "kmalloc.h"     // For dynamic memory allocation
#include "string.h"      // For string operations
#include "types.h"       // Centralized type definitions

/* Maximum file descriptors per process */
#define MAX_FD 256

/**
 * sys_file_t
 *
 * Represents an open file in the kernel.
 */
typedef struct sys_file {
    int fd;                // File descriptor number
    file_t *file;          // Underlying file structure from the VFS layer
    int flags;             // Open flags (inherited from VFS)
    off_t pos;             // Current file offset
    int ref_count;         // Reference count (for duplicates)
} sys_file_t;

/* Global file descriptor table.
   In a production system, this table would be part of the process control block.
*/
static sys_file_t *fd_table[MAX_FD] = {0};

/**
 * allocate_fd - Find an available file descriptor slot.
 */
static int allocate_fd(sys_file_t *sf) {
    for (int i = 0; i < MAX_FD; i++) {
        if (fd_table[i] == NULL) {
            fd_table[i] = sf;
            return i;
        }
    }
    return -1;
}

/**
 * sys_open - Open a file given a pathname.
 */
int sys_open(const char *pathname, int flags, int mode) {
    if (!pathname) {
        terminal_write("[sys_open] Invalid pathname pointer.\n");
        return -1;
    }
    
    /* Call VFS open with the pathname and flags.
     * The extra mode parameter is not used by vfs_open.
     */
    file_t *vfile = vfs_open(pathname, flags);
    if (!vfile) {
        terminal_write("[sys_open] vfs_open failed.\n");
        return -1;
    }
    
    sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
    if (!sf) {
        terminal_write("[sys_open] Out of memory for sys_file_t.\n");
        vfs_close(vfile);
        return -1;
    }
    
    sf->file = vfile;
    sf->flags = flags;
    sf->pos = 0;
    sf->ref_count = 1;
    
    int fd = allocate_fd(sf);
    if (fd < 0) {
        terminal_write("[sys_open] No available file descriptor.\n");
        vfs_close(vfile);
        kfree(sf);
        return -1;
    }
    sf->fd = fd;
    return fd;
}

/**
 * sys_read - Read from a file descriptor.
 */
ssize_t sys_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd] == NULL) {
        terminal_write("[sys_read] Invalid file descriptor.\n");
        return -1;
    }
    
    sys_file_t *sf = fd_table[fd];
    ssize_t bytes = vfs_read(sf->file, buf, count);
    if (bytes > 0) {
        sf->pos += bytes;
    }
    return bytes;
}

/**
 * sys_write - Write to a file descriptor.
 */
ssize_t sys_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd] == NULL) {
        terminal_write("[sys_write] Invalid file descriptor.\n");
        return -1;
    }
    
    sys_file_t *sf = fd_table[fd];
    ssize_t bytes = vfs_write(sf->file, buf, count);
    if (bytes > 0) {
        sf->pos += bytes;
    }
    return bytes;
}

/**
 * sys_close - Close a file descriptor.
 */
int sys_close(int fd) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd] == NULL) {
        terminal_write("[sys_close] Invalid file descriptor.\n");
        return -1;
    }
    
    sys_file_t *sf = fd_table[fd];
    int ret = vfs_close(sf->file);
    kfree(sf);
    fd_table[fd] = NULL;
    return ret;
}

/**
 * sys_lseek - Reposition the file offset.
 */
off_t sys_lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd] == NULL) {
        terminal_write("[sys_lseek] Invalid file descriptor.\n");
        return -1;
    }
    
    sys_file_t *sf = fd_table[fd];
    off_t new_pos = vfs_lseek(sf->file, offset, whence);
    if (new_pos >= 0) {
        sf->pos = new_pos;
    }
    return new_pos;
}
