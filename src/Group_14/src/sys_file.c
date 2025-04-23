/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer
 *
 * Implements the kernel-side logic for file-related system calls (open, read,
 * write, close, lseek). This layer acts as an intermediary between the syscall
 * dispatcher and the Virtual File System (VFS). It manages the process-specific
 * file descriptor table and translates file descriptors to underlying VFS file objects.
 */

 #include "sys_file.h"   // Includes definition of sys_file_t, flags (O_*), SEEK_*
 #include "vfs.h"        // Provides vfs_* functions and file_t definition
 #include "terminal.h"   // For debug output
 #include "kmalloc.h"    // For kmalloc, kfree
 #include "string.h"     // For memset (though often not needed if kmalloc zeroes)
 #include "types.h"      // Centralized type definitions (ssize_t, off_t, size_t)
 #include "fs_errno.h"   // Standard error codes (EBADF, ENOMEM, EMFILE, EFAULT, etc.)
 #include "fs_limits.h"  // For MAX_FD
 #include "process.h"    // For pcb_t, get_current_process() and fd_table access
 #include "assert.h"     // For KERNEL_ASSERT
 #include "debug.h"      // For DEBUG_PRINTK
 
 // --- Debug Configuration ---
 #define DEBUG_SYSFILE 0 // Set to 1 to enable sys_file debug messages
 
 #if DEBUG_SYSFILE
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[SysFile] " fmt, ##__VA_ARGS__)
 #else
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif
 
 //-----------------------------------------------------------------------------
 // Internal Helper Functions (Static)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Allocates a file descriptor for the current process.
  * Finds the lowest available index in the process's file descriptor table.
  * Associates the given sys_file structure with the allocated FD.
  *
  * @param proc Pointer to the current process control block. Must not be NULL.
  * @param sf Pointer to the allocated sys_file_t structure to store in the table. Must not be NULL.
  * @return The allocated file descriptor number (>= 0) on success.
  * @return -EMFILE if the process's file descriptor table is full.
  * @note For SMP, access to proc->fd_table needs locking.
  */
 static int allocate_fd_for_process(pcb_t *proc, sys_file_t *sf) {
     KERNEL_ASSERT(proc != NULL, "NULL process in allocate_fd");
     KERNEL_ASSERT(sf != NULL, "NULL sys_file in allocate_fd");
 
     // TODO: SMP Lock: Acquire lock protecting proc->fd_table
 
     // *** Requires pcb_t definition in process.h to include: ***
     //     struct sys_file *fd_table[MAX_FD];
     // *** Ensure process_init_fds in process.c NULLs this table initially. ***
 
     for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf;
             // TODO: SMP Lock: Release lock protecting proc->fd_table
             SYSFILE_DEBUG_PRINTK("PID %lu: Allocated fd %d\n", proc->pid, fd);
             return fd; // Return the index as the file descriptor
         }
     }
 
     // TODO: SMP Lock: Release lock protecting proc->fd_table
     SYSFILE_DEBUG_PRINTK("PID %lu: Failed to allocate FD (-EMFILE)\n", proc->pid);
     return -EMFILE; // No file descriptors available
 }
 
 /**
  * @brief Retrieves the sys_file_t structure for a given FD for the current process.
  * Performs bounds checking and checks if the FD slot holds an open file structure.
  *
  * @param proc Pointer to the current process control block. Must not be NULL.
  * @param fd The file descriptor number to look up.
  * @return Pointer to the valid sys_file_t structure on success.
  * @return NULL if the file descriptor is invalid (out of bounds or not open).
  * @note For SMP, access to proc->fd_table needs locking (at least for read).
  */
 static sys_file_t* get_sys_file(pcb_t *proc, int fd) {
     KERNEL_ASSERT(proc != NULL, "NULL process in get_sys_file");
 
     // TODO: SMP Lock: Acquire lock protecting proc->fd_table (can be read lock)
 
     // Basic bounds check
     if (fd < 0 || fd >= MAX_FD) {
         // TODO: SMP Lock: Release lock protecting proc->fd_table
         SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (out of bounds)\n", proc->pid, fd);
         return NULL;
     }
 
     // Check if the FD slot actually points to an open file structure
     sys_file_t *sf = proc->fd_table[fd];
 
     // TODO: SMP Lock: Release lock protecting proc->fd_table
 
     if (sf == NULL) {
          SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (not open)\n", proc->pid, fd);
         return NULL; // FD is within bounds, but not currently open.
     }
 
     // Check if the underlying VFS file pointer is valid (internal consistency)
     KERNEL_ASSERT(sf->vfs_file != NULL, "sys_file_t has NULL vfs_file pointer!");
 
     return sf;
 }
 
 //-----------------------------------------------------------------------------
 // System Call Backend Functions (Called by syscall dispatcher)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Kernel implementation for opening or creating a file.
  * Bridges the open() syscall to the VFS layer.
  *
  * @param pathname Path to the file (already in kernel space). Must not be NULL.
  * @param flags File open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.).
  * @param mode Permissions mode (used if O_CREAT is specified).
  * @return File descriptor (int >= 0) on success.
  * @return Negative errno (from fs_errno.h) on failure.
  */
 int sys_open(const char *pathname, int flags, int mode) {
     KERNEL_ASSERT(pathname != NULL, "NULL pathname passed to sys_open");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         // This should ideally be caught by the syscall dispatcher, but double-check.
         SYSFILE_DEBUG_PRINTK("Error: Cannot sys_open without process context.\n");
         return -EFAULT; // Process context required
     }
      SYSFILE_DEBUG_PRINTK("PID %lu: sys_open(path='%s', flags=0x%x, mode=0%o)\n",
                          current_proc->pid, pathname, flags, mode);
 
     // 1. Call VFS to open/create the file. VFS handles path traversal, permissions etc.
     //    We expect vfs_open to return NULL on failure and potentially set an internal errno,
     //    or ideally, return a negative errno directly (if VFS is designed that way).
     file_t *vfile = vfs_open(pathname, flags); // Mode might be used internally by vfs_open if O_CREAT
 
     // Check if VFS open failed
     if (!vfile) {
         // TODO: Ideally, vfs_open would return the specific negative errno.
         // If not, we might have to guess or return a generic error.
         // Common errors: -ENOENT (not found), -EACCES (permission), -EISDIR, etc.
         SYSFILE_DEBUG_PRINTK(" -> vfs_open failed for path '%s'. Returning -ENOENT.\n", pathname);
         return -ENOENT; // Assume file not found as a common case
     }
     SYSFILE_DEBUG_PRINTK(" -> vfs_open succeeded, vfile=%p\n", vfile);
 
 
     // 2. Allocate kernel structure (sys_file_t) to represent the open file description.
     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) {
         SYSFILE_DEBUG_PRINTK(" -> kmalloc failed for sys_file_t. Returning -ENOMEM.\n");
         vfs_close(vfile); // Must close the successfully opened VFS file before returning
         return -ENOMEM;
     }
     // Initialize the structure (memset might be redundant if kmalloc zeroes)
     // memset(sf, 0, sizeof(sys_file_t)); // Optional
     sf->vfs_file = vfile;
     sf->flags = flags; // Store the flags used to open the file
 
     // 3. Allocate a file descriptor number in the process's table.
     int fd = allocate_fd_for_process(current_proc, sf);
 
     // Check if FD allocation failed
     if (fd < 0) {
         // Error should be -EMFILE from allocate_fd_for_process
         SYSFILE_DEBUG_PRINTK(" -> allocate_fd_for_process failed (%d).\n", fd);
         vfs_close(vfile); // Clean up VFS file
         kfree(sf);        // Clean up allocated sys_file struct
         return fd;        // Return the error (-EMFILE)
     }
 
     // Success!
     SYSFILE_DEBUG_PRINTK(" -> Success. Assigned fd %d.\n", fd);
     return fd; // Return the allocated file descriptor number
 }
 
 /**
  * @brief Kernel implementation for reading from an open file descriptor.
  * Bridges the read() syscall to the VFS layer.
  *
  * @param fd File descriptor number.
  * @param kbuf Kernel buffer to store read data. Must be valid kernel memory.
  * @param count Maximum number of bytes to read into `kbuf`.
  * @return Number of bytes read (>= 0) on success (0 indicates EOF).
  * @return Negative errno on failure (e.g., -EBADF, -EIO).
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_read with count > 0");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         SYSFILE_DEBUG_PRINTK("Error: Cannot sys_read without process context.\n");
         return -EFAULT;
     }
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_read(fd=%d, kbuf=%p, count=%u)\n", current_proc->pid, fd, kbuf, count);
 
     // Get the sys_file structure corresponding to the FD
     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid fd %d (-EBADF)\n", fd);
         return -EBADF; // Bad file descriptor
     }
 
     // --- Check File Permissions ---
     // Ensure the file was opened with read access.
     // Access modes are in the lower bits (O_RDONLY=0, O_RDWR=2).
     if ((sf->flags & O_ACCMODE) != O_RDONLY && (sf->flags & O_ACCMODE) != O_RDWR) {
          SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for reading (flags=0x%x, -EBADF)\n", fd, sf->flags);
          return -EBADF; // Operation not permitted / Bad FD for this op
     }
 
     // If count is 0, return 0 immediately (POSIX standard)
     if (count == 0) {
         SYSFILE_DEBUG_PRINTK(" -> Success (count is 0)\n");
         return 0;
     }
 
     // Call the VFS read function. VFS handles file position updates.
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
 
     SYSFILE_DEBUG_PRINTK(" -> vfs_read returned %d\n", bytes_read);
     return bytes_read; // Return result from VFS (bytes read or negative errno)
 }
 
 /**
  * @brief Kernel implementation for writing to an open file descriptor.
  * Bridges the write() syscall to the VFS layer.
  *
  * @param fd File descriptor number.
  * @param kbuf Kernel buffer containing data to write. Must be valid kernel memory.
  * @param count Number of bytes to write from `kbuf`.
  * @return Number of bytes written (>= 0) on success. May be less than `count`.
  * @return Negative errno on failure (e.g., -EBADF, -ENOSPC, -EIO).
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_write with count > 0");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         SYSFILE_DEBUG_PRINTK("Error: Cannot sys_write without process context.\n");
         return -EFAULT;
     }
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_write(fd=%d, kbuf=%p, count=%u)\n", current_proc->pid, fd, kbuf, count);
 
 
     // Get the sys_file structure
     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid fd %d (-EBADF)\n", fd);
         return -EBADF; // Bad file descriptor
     }
 
     // --- Check File Permissions ---
     // Ensure the file was opened with write access.
     // Access modes: O_WRONLY=1, O_RDWR=2.
     if ((sf->flags & O_ACCMODE) != O_WRONLY && (sf->flags & O_ACCMODE) != O_RDWR) {
          SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for writing (flags=0x%x, -EBADF)\n", fd, sf->flags);
          return -EBADF; // Operation not permitted / Bad FD for this op
     }
 
     // If count is 0, return 0 immediately
     if (count == 0) {
         SYSFILE_DEBUG_PRINTK(" -> Success (count is 0)\n");
         return 0;
     }
 
     // Call the VFS write function. VFS handles O_APPEND flag and file position updates.
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
 
     SYSFILE_DEBUG_PRINTK(" -> vfs_write returned %d\n", bytes_written);
     return bytes_written; // Return result from VFS (bytes written or negative errno)
 }
 
 /**
  * @brief Kernel implementation for closing an open file descriptor.
  * Bridges the close() syscall to the VFS layer.
  *
  * @param fd File descriptor number to close.
  * @return 0 on success.
  * @return Negative errno on failure (e.g., -EBADF).
  */
 int sys_close(int fd) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         SYSFILE_DEBUG_PRINTK("Error: Cannot sys_close without process context.\n");
         return -EFAULT;
     }
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_close(fd=%d)\n", current_proc->pid, fd);
 
     // Get the sys_file structure, ensuring FD is valid and open.
     // Need pointer to clear table entry. Using index only isn't enough.
     // TODO: SMP Lock: Acquire write lock for fd_table
     sys_file_t *sf = get_sys_file(current_proc, fd); // Assumes get_sys_file handles locking correctly if needed internally
     if (!sf) {
         // TODO: SMP Lock: Release lock
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid fd %d (-EBADF)\n", fd);
         return -EBADF; // Bad file descriptor
     }
 
     // Mark the FD table entry as free *before* potentially blocking in vfs_close or kfree.
     // This prevents race conditions where another thread might try to reuse the FD
     // while cleanup is still in progress (important for SMP).
     KERNEL_ASSERT(current_proc->fd_table[fd] == sf, "FD table inconsistency during close");
     current_proc->fd_table[fd] = NULL;
     // TODO: SMP Lock: Release lock for fd_table
 
     SYSFILE_DEBUG_PRINTK("   Cleared fd_table[%d]. Closing VFS file %p.\n", fd, sf->vfs_file);
 
 
     // Close the underlying VFS file object. This should handle flushing buffers etc.
     int vfs_ret = vfs_close(sf->vfs_file);
     if (vfs_ret < 0) {
         // VFS close failed. This is unusual. Log it. The FD is already closed from the process's perspective.
          SYSFILE_DEBUG_PRINTK("   Warning: vfs_close for fd %d returned error %d.\n", fd, vfs_ret);
          // We still proceed to free the sys_file_t structure.
     } else {
          SYSFILE_DEBUG_PRINTK("   vfs_close succeeded.\n");
     }
 
 
     // Free the sys_file kernel structure itself.
     kfree(sf);
 
     SYSFILE_DEBUG_PRINTK(" -> Success (fd %d closed).\n", fd);
     return 0; // POSIX close typically returns 0 on success, even if underlying device had issues.
               // Returning vfs_ret might be more informative but less standard.
 }
 
 /**
  * @brief Kernel implementation for repositioning the read/write file offset.
  * Bridges the lseek() syscall to the VFS layer.
  *
  * @param fd File descriptor number.
  * @param offset Offset value relative to `whence`. Can be negative.
  * @param whence Directive for positioning: SEEK_SET, SEEK_CUR, or SEEK_END.
  * @return The resulting offset location from the beginning of the file (>= 0) on success.
  * @return Negative errno on failure (e.g., -EBADF, -EINVAL, -ESPIPE).
  */
 off_t sys_lseek(int fd, off_t offset, int whence) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
          SYSFILE_DEBUG_PRINTK("Error: Cannot sys_lseek without process context.\n");
         return -EFAULT;
     }
      SYSFILE_DEBUG_PRINTK("PID %lu: sys_lseek(fd=%d, offset=%ld, whence=%d)\n",
                           current_proc->pid, fd, (long)offset, whence); // Use %ld for off_t
 
     // Get the sys_file structure
     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid fd %d (-EBADF)\n", fd);
         return -EBADF; // Bad file descriptor
     }
 
     // Basic validation for whence (also done in syscall dispatcher, belt-and-suspenders)
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid whence %d (-EINVAL)\n", whence);
         return -EINVAL;
     }
 
     // Call the VFS lseek function. VFS handles checks for seekable files (e.g., pipes).
     off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
 
     // vfs_lseek returns the new position (>=0) or a negative errno (e.g., -ESPIPE, -EINVAL)
     SYSFILE_DEBUG_PRINTK(" -> vfs_lseek returned %ld\n", (long)new_pos);
     return new_pos;
 }