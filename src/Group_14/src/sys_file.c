/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer (Corrected v1.1)
 *
 * Implements the backend logic for file-related system calls (open, read,
 * write, close, lseek). Interacts with the VFS layer and manages per-process
 * file descriptor tables.
 *
 * Version 1.1 Changes:
 * - Corrected access mode permission checks in sys_read and sys_write.
 * - Added comments indicating where locking for the per-process FD table is required (assuming a lock exists within pcb_t).
 * - Ensured use of standard POSIX errno values defined in fs_errno.h.
 */

 #include "sys_file.h"
 #include "vfs.h"
 #include "terminal.h"       // Logging
 #include "kmalloc.h"        // kmalloc, kfree
 #include "string.h"         // Kernel string functions
 #include "types.h"          // Core types (ssize_t, off_t, etc.)
 #include "fs_errno.h"       // Filesystem/POSIX error codes (EBADF, EINVAL, EACCES etc.)
 #include "fs_limits.h"      // MAX_FD definition
 #include "process.h"        // pcb_t, get_current_process
 #include "assert.h"         // KERNEL_ASSERT
 #include "debug.h"          // DEBUG_PRINTK macros
 #include "serial.h"         // Low-level serial logging for critical paths
 #include "spinlock.h"       // For spinlock type and functions (used conceptually for comments)
 
 // Debugging configuration
 #define DEBUG_SYSFILE 1 // Set to 0 to disable debug prints
 #if DEBUG_SYSFILE
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[SysFile] " fmt "\n", ##__VA_ARGS__)
 #else
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif
 
 //-----------------------------------------------------------------------------
 // Internal Helper Functions (Static)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Finds the lowest available file descriptor slot in a process's table.
  * Assigns the sys_file_t structure to that slot.
  * @param proc The process control block.
  * @param sf The system file structure to assign.
  * @return The allocated file descriptor number (>= 0) on success, or -EMFILE if no slots are available.
  * @note **Requires external locking** on the process's fd_table before calling.
  */
 static int allocate_fd_for_process(pcb_t *proc, sys_file_t *sf) {
     KERNEL_ASSERT(proc != NULL, "NULL process in allocate_fd");
     KERNEL_ASSERT(sf != NULL, "NULL sys_file in allocate_fd");
     // ---> LOCKING: Acquire write lock on proc->fd_table_lock here <---
     for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf;
             SYSFILE_DEBUG_PRINTK("PID %lu: Allocated fd %d", (unsigned long)proc->pid, fd);
             // ---> LOCKING: Release write lock on proc->fd_table_lock here <---
             return fd;
         }
     }
     // ---> LOCKING: Release write lock on proc->fd_table_lock here <---
     SYSFILE_DEBUG_PRINTK("PID %lu: Failed to allocate FD (-EMFILE)", (unsigned long)proc->pid);
     return -EMFILE; // Too many open files for this process
 }
 
 /**
  * @brief Retrieves the sys_file_t* for a given FD in a process's table.
  * Performs bounds checking and ensures the FD corresponds to an open file.
  * @param proc The process control block.
  * @param fd The file descriptor number.
  * @return Pointer to the sys_file_t structure on success, NULL if the FD is invalid or not open.
  * @note **Requires external locking** (read lock sufficient) on the process's fd_table before calling.
  */
 static sys_file_t* get_sys_file(pcb_t *proc, int fd) {
     KERNEL_ASSERT(proc != NULL, "NULL process in get_sys_file");
     // ---> LOCKING: Acquire read lock on proc->fd_table_lock here <---
     if (fd < 0 || fd >= MAX_FD) {
         SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (out of bounds)", (unsigned long)proc->pid, fd);
         // ---> LOCKING: Release read lock on proc->fd_table_lock here <---
         return NULL;
     }
     sys_file_t *sf = proc->fd_table[fd];
     // ---> LOCKING: Release read lock on proc->fd_table_lock here <---
 
     if (sf == NULL) {
         SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (not open)", (unsigned long)proc->pid, fd);
         return NULL;
     }
     KERNEL_ASSERT(sf->vfs_file != NULL, "sys_file_t has NULL vfs_file pointer!");
     return sf;
 }
 
 //-----------------------------------------------------------------------------
 // System Call Backend Functions (Called by syscall dispatcher)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Backend implementation for the open() system call.
  * Opens or creates a file using the VFS, allocates a sys_file_t structure
  * and a file descriptor for the current process.
  * @param pathname User-space pointer to the path string.
  * @param flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, etc.).
  * @param mode Permissions mode (used only when O_CREAT is specified).
  * @return File descriptor (>= 0) on success, negative errno value on failure.
  */
 int sys_open(const char *pathname, int flags, int mode) {
     // Serial logging for entry
     serial_write("[sys_open] Enter. Pathname='");
     serial_write(pathname ? pathname : "NULL");
     serial_write("', Flags=0x"); serial_print_hex((uint32_t)flags);
     serial_write(", Mode=0"); serial_print_hex((uint32_t)mode); serial_write("\n");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) { serial_write("[sys_open] Error: No current process. Returning -EFAULT.\n"); return -EFAULT; }
 
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_open(path_user=%p, flags=0x%x, mode=0%o)", (unsigned long)current_proc->pid, pathname, flags, mode);
 
     // Pathname validation happens within the syscall implementation (sys_open_impl)
     // which copies the path safely before calling this backend.
     KERNEL_ASSERT(pathname != NULL, "sys_open called with NULL kernel pathname");
 
     // Call VFS to open the file
     serial_write("[sys_open] Calling vfs_open...\n");
     file_t *vfile = vfs_open(pathname, flags);
     serial_write("[sys_open] vfs_open returned vfile="); serial_print_hex((uintptr_t)vfile); serial_write("\n");
 
     if (!vfile) {
         SYSFILE_DEBUG_PRINTK(" -> vfs_open failed for path '%s'. Returning -ENOENT (assumed).", pathname);
         serial_write("[sys_open] vfs_open failed. Returning -ENOENT.\n");
         // TODO: VFS should ideally return a more specific error code (EACCES, ENOTDIR, etc.)
         return -ENOENT;
     }
     SYSFILE_DEBUG_PRINTK(" -> vfs_open succeeded, vfile=%p", vfile);
     serial_write("[sys_open] vfs_open succeeded.\n");
 
     // Allocate the kernel-level file structure
     serial_write("[sys_open] Allocating sys_file_t...\n");
     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) {
         SYSFILE_DEBUG_PRINTK(" -> kmalloc failed for sys_file_t. Returning -ENOMEM.");
         serial_write("[sys_open] kmalloc for sys_file_t failed. Closing vfile, returning -ENOMEM.\n");
         vfs_close(vfile); // Clean up the opened VFS file
         return -ENOMEM;
     }
     sf->vfs_file = vfile;
     sf->flags = flags; // Store the open flags for permission checks later
     serial_write("[sys_open] sys_file_t allocated and populated.\n");
 
     // Allocate a file descriptor for the process
     serial_write("[sys_open] Allocating file descriptor...\n");
     // allocate_fd_for_process requires lock on fd_table
     // ---> LOCKING: Acquire write lock on current_proc->fd_table_lock here <---
     int fd = allocate_fd_for_process(current_proc, sf);
     // ---> LOCKING: Release write lock on current_proc->fd_table_lock here <---
     if (fd < 0) { // -EMFILE
         SYSFILE_DEBUG_PRINTK(" -> allocate_fd_for_process failed (%d).", fd);
         serial_write("[sys_open] allocate_fd failed. Closing vfile, freeing sf, returning error.\n");
         vfs_close(vfile);
         kfree(sf);
         return fd; // Return -EMFILE
     }
 
     SYSFILE_DEBUG_PRINTK(" -> Success. Assigned fd %d.", fd);
     serial_write("[sys_open] FD allocated: "); serial_print_hex((uint32_t)fd); serial_write(". Returning fd.\n");
     return fd;
 }
 
 /**
  * @brief Backend implementation for the read() system call.
  * Reads data from an open file descriptor into a kernel buffer.
  * @param fd The file descriptor.
  * @param kbuf Kernel buffer to read data into.
  * @param count Maximum number of bytes to read.
  * @return Number of bytes read on success, 0 on EOF, negative errno on failure.
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_read with count > 0");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT; // Should not happen in syscall context
 
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_read(fd=%d, kbuf=%p, count=%zu)", (unsigned long)current_proc->pid, fd, kbuf, count);
 
     // Retrieve sys_file_t safely
     // ---> LOCKING: Acquire read lock on current_proc->fd_table_lock here <---
     sys_file_t *sf = get_sys_file(current_proc, fd);
     // ---> LOCKING: Release read lock on current_proc->fd_table_lock here <---
     if (!sf) return -EBADF; // Bad file descriptor
 
     // --- CORRECTED Permission Check ---
     // Check if the file was opened with read access (O_RDONLY or O_RDWR)
     int access_mode = sf->flags & O_ACCMODE;
     if (access_mode != O_RDONLY && access_mode != O_RDWR) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for reading (flags=0x%x, access_mode=0x%x, -EACCES)", fd, sf->flags, access_mode);
         return -EACCES; // Permission denied (or EBADF arguably)
     }
     // --- End Correction ---
 
     if (count == 0) return 0; // Read 0 bytes successfully
 
     // Call VFS layer
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     SYSFILE_DEBUG_PRINTK(" -> vfs_read returned %zd", bytes_read);
     return bytes_read; // Return bytes read or negative error code from VFS
 }
 
 /**
  * @brief Backend implementation for the write() system call.
  * Writes data from a kernel buffer to an open file descriptor.
  * @param fd The file descriptor.
  * @param kbuf Kernel buffer containing data to write.
  * @param count Number of bytes to write.
  * @return Number of bytes written on success, negative errno on failure.
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_write with count > 0");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_write(fd=%d, kbuf=%p, count=%zu)", (unsigned long)current_proc->pid, fd, kbuf, count);
 
     // Retrieve sys_file_t safely
     // ---> LOCKING: Acquire read lock on current_proc->fd_table_lock here <---
     sys_file_t *sf = get_sys_file(current_proc, fd);
     // ---> LOCKING: Release read lock on current_proc->fd_table_lock here <---
     if (!sf) return -EBADF; // Bad file descriptor
 
     // --- CORRECTED Permission Check ---
     // Check if the file was opened with write access (O_WRONLY or O_RDWR)
     int access_mode = sf->flags & O_ACCMODE;
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for writing (flags=0x%x, access_mode=0x%x, -EACCES)", fd, sf->flags, access_mode);
         return -EACCES; // Permission denied (or EBADF)
     }
     // --- End Correction ---
 
     if (count == 0) return 0; // Wrote 0 bytes successfully
 
     // Call VFS layer
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     SYSFILE_DEBUG_PRINTK(" -> vfs_write returned %zd", bytes_written);
     return bytes_written; // Return bytes written or negative error code from VFS
 }
 
 /**
  * @brief Backend implementation for the close() system call.
  * Closes a file descriptor for the current process, releasing associated resources.
  * @param fd The file descriptor to close.
  * @return 0 on success, negative errno on failure.
  */
 int sys_close(int fd) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_close(fd=%d)", (unsigned long)current_proc->pid, fd);
 
     // --- Critical Section for FD Table Modification ---
     // ---> LOCKING: Acquire write lock on current_proc->fd_table_lock here <---
     sys_file_t *sf = get_sys_file(current_proc, fd); // Need lock for get_sys_file consistency too
     if (!sf) {
         // ---> LOCKING: Release write lock on current_proc->fd_table_lock here <---
         return -EBADF; // Bad file descriptor
     }
 
     // Clear FD table entry FIRST while holding the lock
     KERNEL_ASSERT(current_proc->fd_table[fd] == sf, "FD table inconsistency during close");
     current_proc->fd_table[fd] = NULL;
     // ---> LOCKING: Release write lock on current_proc->fd_table_lock here <---
     // --- End Critical Section ---
 
     SYSFILE_DEBUG_PRINTK("   Cleared fd_table[%d]. Closing VFS file %p.", fd, sf->vfs_file);
 
     // Call VFS close (safe to call outside lock now that FD entry is clear)
     int vfs_ret = vfs_close(sf->vfs_file); // vfs_close handles freeing sf->vfs_file->data
     if (vfs_ret < 0) {
         SYSFILE_DEBUG_PRINTK("   Warning: vfs_close for fd %d returned error %d.", fd, vfs_ret);
         // POSIX close should generally succeed even if underlying flush fails,
         // but we might propagate the error if it's critical. Let's return 0 for now.
     } else {
         SYSFILE_DEBUG_PRINTK("   vfs_close succeeded.");
     }
 
     // Free the sys_file structure itself
     kfree(sf);
 
     SYSFILE_DEBUG_PRINTK(" -> Success (fd %d closed).", fd);
     return 0; // POSIX standard is 0 on success
 }
 
 /**
  * @brief Backend implementation for the lseek() system call.
  * Repositions the read/write offset of an open file descriptor.
  * @param fd The file descriptor.
  * @param offset Offset value (interpretation depends on whence).
  * @param whence Reference point for the seek (SEEK_SET, SEEK_CUR, SEEK_END).
  * @return The resulting offset from the beginning of the file on success,
  * negative errno value on failure.
  */
 off_t sys_lseek(int fd, off_t offset, int whence) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_lseek(fd=%d, offset=%ld, whence=%d)", (unsigned long)current_proc->pid, fd, (long)offset, whence);
 
     // Retrieve sys_file_t safely
     // ---> LOCKING: Acquire read lock on current_proc->fd_table_lock here <---
     sys_file_t *sf = get_sys_file(current_proc, fd);
     // ---> LOCKING: Release read lock on current_proc->fd_table_lock here <---
     if (!sf) return -EBADF;
 
     // Validate whence parameter
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid whence %d (-EINVAL)", whence);
         return -EINVAL;
     }
 
     // Call VFS layer (VFS handles offset update and returns new absolute offset or error)
     off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
     SYSFILE_DEBUG_PRINTK(" -> vfs_lseek returned %ld", (long)new_pos);
 
     // Check for negative return value from vfs_lseek, which indicates an error
     if (new_pos < 0) {
         // Map VFS errors to appropriate negative errno values if necessary
         // Assuming vfs_lseek already returns negative errno values like -EINVAL or -ESPIPE
         return new_pos;
     }
 
     // Return the new absolute offset
     return new_pos;
 }