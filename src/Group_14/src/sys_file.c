#include "sys_file.h"   // Now includes the definition of sys_file_t
#include "vfs.h"        // Provides vfs_open, vfs_read, vfs_write, vfs_close, vfs_lseek, file_t
#include "terminal.h"   // For diagnostic output
#include "kmalloc.h"    // For dynamic memory allocation
#include "string.h"     // For memset
#include "types.h"      // Centralized type definitions
#include "fs_errno.h"   // Error codes (EBADF, ENOMEM, EMFILE, EFAULT, ENOENT)
#include "fs_limits.h"  // For MAX_FD
#include "process.h"    // For pcb_t, get_current_process()
#include "assert.h"     // For KERNEL_ASSERT


// NOTE: The definition of sys_file_t has been moved to sys_file.h


//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

/**
 * @brief Allocates a file descriptor for the current process.
 * Finds the lowest available index in the process's fd_table.
 *
 * @param proc The current process control block.
 * @param sf Pointer to the allocated sys_file_t structure to store.
 * @return The allocated file descriptor number (>= 0) on success, or -EMFILE if no FDs are available.
 */
static int allocate_fd_for_process(pcb_t *proc, sys_file_t *sf) {
    KERNEL_ASSERT(proc != NULL, "NULL process in allocate_fd");
    KERNEL_ASSERT(sf != NULL, "NULL sys_file in allocate_fd");

    // *** IMPORTANT: Assumes pcb_t in process.h has member `struct sys_file *fd_table[MAX_FD];` ***
    for (int i = 0; i < MAX_FD; i++) {
        if (proc->fd_table[i] == NULL) {
            proc->fd_table[i] = sf;
            return i; // Return the index as the file descriptor
        }
    }
    return -EMFILE; // No file descriptors available
}

/**
 * @brief Retrieves the sys_file_t structure for a given FD for the current process.
 * Performs bounds checking and checks if the FD is actually open.
 *
 * @param proc The current process control block.
 * @param fd The file descriptor number.
 * @return Pointer to the sys_file_t structure on success, NULL on failure (invalid FD).
 */
static sys_file_t* get_sys_file(pcb_t *proc, int fd) {
    KERNEL_ASSERT(proc != NULL, "NULL process in get_sys_file");

    // *** IMPORTANT: Assumes pcb_t in process.h has member `struct sys_file *fd_table[MAX_FD];` ***
    if (fd < 0 || fd >= MAX_FD || proc->fd_table[fd] == NULL) {
        return NULL; // Invalid file descriptor
    }
    return proc->fd_table[fd];
}

//-----------------------------------------------------------------------------
// System Call Implementation Functions (called by syscall_handler)
//-----------------------------------------------------------------------------

/**
 * @brief Opens or creates a file. Corresponds to the open() syscall.
 *
 * @param pathname Path to the file.
 * @param flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_APPEND, etc.).
 * @param mode Permissions mode (used if O_CREAT is specified, often ignored in simple FS).
 * @return File descriptor (>= 0) on success, negative errno on failure.
 */
int sys_open(const char *pathname, int flags, int mode) {
    pcb_t *current_proc = get_current_process();
    if (!current_proc) {
        terminal_write("[sys_open] Error: Cannot open file without process context.\n");
        return -EFAULT; // Or appropriate error
    }

    // VFS handles path validation and file opening/creation
    file_t *vfile = vfs_open(pathname, flags);
    if (!vfile) {
        // vfs_open should return NULL and set errno appropriately,
        // but we return a generic error for now if it fails.
        terminal_printf("[sys_open] vfs_open failed for path '%s'.\n", pathname);
        return -ENOENT; // Or EACCES, etc., depending on VFS failure reason
    }

    // Allocate kernel structure to track this open file instance
    sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
    if (!sf) {
        terminal_write("[sys_open] Out of memory for sys_file_t.\n");
        vfs_close(vfile); // Clean up the opened VFS file
        return -ENOMEM;
    }

    // Initialize the sys_file structure
    sf->vfs_file = vfile;
    sf->flags = flags; // Store flags

    // Allocate a file descriptor number for the current process
    int fd = allocate_fd_for_process(current_proc, sf);
    if (fd < 0) {
        // No FD available (-EMFILE)
        terminal_write("[sys_open] No available file descriptor for process.\n");
        vfs_close(vfile);
        kfree(sf);
        return fd; // Return the error code from allocate_fd_for_process (-EMFILE)
    }

    // terminal_printf("[sys_open] Opened '%s' as fd %d\n", pathname, fd);
    return fd; // Return the file descriptor number
}

/**
 * @brief Reads data from an open file descriptor. Corresponds to read() syscall.
 *
 * @param fd File descriptor number.
 * @param kbuf Kernel buffer to store read data.
 * @param count Maximum number of bytes to read.
 * @return Number of bytes read (>= 0) on success (0 indicates EOF), negative errno on failure.
 */
ssize_t sys_read(int fd, void *kbuf, size_t count) {
    pcb_t *current_proc = get_current_process();
    if (!current_proc) return -EFAULT;

    sys_file_t *sf = get_sys_file(current_proc, fd);
    if (!sf) {
        return -EBADF; // Bad file descriptor
    }

    // Optional: Check read permissions based on sf->flags
    // if (!(sf->flags & O_RDONLY) && !(sf->flags & O_RDWR)) return -EBADF;

    // Call VFS read function.
    ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);

    // terminal_printf("[sys_read] fd=%d, count=%lu -> read %ld bytes\n", fd, count, (long)bytes_read);
    return bytes_read;
}

/**
 * @brief Writes data to an open file descriptor. Corresponds to write() syscall.
 *
 * @param fd File descriptor number.
 * @param kbuf Kernel buffer containing data to write.
 * @param count Number of bytes to write.
 * @return Number of bytes written (>= 0) on success, negative errno on failure.
 */
ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;

     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) {
         return -EBADF; // Bad file descriptor
     }

     // Optional: Check write permissions based on sf->flags
     // if (!(sf->flags & O_WRONLY) && !(sf->flags & O_RDWR)) return -EBADF;

     // Call VFS write function.
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);

     // terminal_printf("[sys_write] fd=%d, count=%lu -> wrote %ld bytes\n", fd, count, (long)bytes_written);
     return bytes_written;
}

/**
 * @brief Closes an open file descriptor. Corresponds to close() syscall.
 *
 * @param fd File descriptor number to close.
 * @return 0 on success, negative errno on failure.
 */
int sys_close(int fd) {
    pcb_t *current_proc = get_current_process();
    if (!current_proc) return -EFAULT;

    sys_file_t *sf = get_sys_file(current_proc, fd);
    if (!sf) {
        return -EBADF; // Bad file descriptor
    }

    // Clear the entry in the process's FD table *first*
    // *** IMPORTANT: Assumes pcb_t has member `struct sys_file *fd_table[MAX_FD];` ***
    current_proc->fd_table[fd] = NULL;

    // Close the underlying VFS file
    int vfs_ret = vfs_close(sf->vfs_file);

    // Free the sys_file kernel structure
    kfree(sf);

    // terminal_printf("[sys_close] Closed fd %d\n", fd);
    return vfs_ret; // Return the result from vfs_close
}

/**
 * @brief Repositions the read/write file offset. Corresponds to lseek() syscall.
 *
 * @param fd File descriptor number.
 * @param offset Offset value.
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END.
 * @return The resulting offset location from the beginning of the file on success,
 * negative errno on failure.
 */
off_t sys_lseek(int fd, off_t offset, int whence) {
    pcb_t *current_proc = get_current_process();
    if (!current_proc) return -EFAULT;

    sys_file_t *sf = get_sys_file(current_proc, fd);
    if (!sf) {
        return -EBADF; // Bad file descriptor
    }

    // Validate whence (already done in syscall_handler, but good practice)
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        return -EINVAL;
    }

    // Call VFS lseek function
    off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);

    // terminal_printf("[sys_lseek] fd=%d, offset=%ld, whence=%d -> new_pos=%ld\n", fd, (long)offset, whence, (long)new_pos);
    return new_pos;
}

// Note: process_init_fds and process_close_fds should reside in process.c

