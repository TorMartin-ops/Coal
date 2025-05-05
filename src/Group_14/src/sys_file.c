/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer (v1.2 - FD Table Lock)
 * @version 1.2
 *
 * Implements the backend logic for file-related system calls (open, read,
 * write, close, lseek). Interacts with the VFS layer and manages per-process
 * file descriptor tables, now with locking to prevent race conditions.
 *
 * Version 1.2 Changes:
 * - Added spinlock protection (proc->fd_table_lock) around all accesses
 * and modifications to the process file descriptor table (fd_table) in
 * allocate_fd_for_process, get_sys_file, and sys_close.
 * - Refined error handling and assertions.
 * - Improved comments regarding locking strategy.
 */

 #include "sys_file.h"
 #include "vfs.h"
 #include "terminal.h"       // Logging
 #include "kmalloc.h"        // kmalloc, kfree
 #include "string.h"         // Kernel string functions
 #include "types.h"          // Core types (ssize_t, off_t, etc.)
 #include "fs_errno.h"       // Filesystem/POSIX error codes (EBADF, EINVAL, EACCES etc.)
 #include "fs_limits.h"      // MAX_FD definition
 #include "process.h"        // pcb_t, get_current_process (includes fd_table_lock now)
 #include "assert.h"         // KERNEL_ASSERT
 #include "debug.h"          // DEBUG_PRINTK macros
 #include "serial.h"         // Low-level serial logging for critical paths
 #include "spinlock.h"       // Spinlock functions and type

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
  * @note Acquires and releases the process's fd_table_lock.
  */
 static int allocate_fd_for_process(pcb_t *proc, sys_file_t *sf) {
     KERNEL_ASSERT(proc != NULL, "NULL process in allocate_fd");
     KERNEL_ASSERT(sf != NULL, "NULL sys_file in allocate_fd");

     int allocated_fd = -EMFILE; // Default to error (Too many open files)

     // === Acquire Lock ===
     uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);

     // Find the first available slot (NULL entry)
     for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf; // Assign the sys_file_t to the slot
             SYSFILE_DEBUG_PRINTK("PID %lu: Allocated fd %d", (unsigned long)proc->pid, fd);
             allocated_fd = fd; // Store the successfully allocated FD
             break; // Found a slot, exit loop
         }
     }

     // === Release Lock ===
     spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

     if (allocated_fd < 0) {
         SYSFILE_DEBUG_PRINTK("PID %lu: Failed to allocate FD (-EMFILE)", (unsigned long)proc->pid);
     }
     return allocated_fd; // Return allocated FD or -EMFILE
 }

 /**
  * @brief Retrieves the sys_file_t* for a given FD in a process's table.
  * Performs bounds checking and ensures the FD corresponds to an open file.
  * @param proc The process control block.
  * @param fd The file descriptor number.
  * @return Pointer to the sys_file_t structure on success, NULL if the FD is invalid or not open.
  * @note Acquires and releases the process's fd_table_lock.
  */
 static sys_file_t* get_sys_file(pcb_t *proc, int fd) {
     KERNEL_ASSERT(proc != NULL, "NULL process in get_sys_file");

     sys_file_t *sf = NULL; // Default to NULL

     // === Acquire Lock ===
     uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);

     // Check bounds first
     if (fd < 0 || fd >= MAX_FD) {
         SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (out of bounds)", (unsigned long)proc->pid, fd);
         // sf remains NULL
     } else {
         sf = proc->fd_table[fd]; // Read the entry while holding the lock
         // Check if the slot actually points to a valid file structure
         if (sf == NULL) {
              SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (not open)", (unsigned long)proc->pid, fd);
              // sf is already NULL
         } else {
             // Basic sanity check on the retrieved structure while lock is held
             KERNEL_ASSERT(sf->vfs_file != NULL, "sys_file_t has NULL vfs_file pointer!");
         }
     }

     // === Release Lock ===
     spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);

     return sf; // Return found pointer or NULL
 }

 //-----------------------------------------------------------------------------
 // System Call Backend Functions (Called by syscall dispatcher)
 //-----------------------------------------------------------------------------

 /**
  * @brief Backend implementation for the open() system call.
  */
 int sys_open(const char *pathname, int flags, int mode) {
     serial_write("[sys_open] Enter. Pathname='");
     serial_write(pathname ? pathname : "NULL");
     serial_write("', Flags=0x"); serial_print_hex((uint32_t)flags);
     serial_write(", Mode=0"); serial_print_hex((uint32_t)mode); serial_write("\n");

     pcb_t *current_proc = get_current_process();
     if (!current_proc) { /* ... error handling ... */ return -EFAULT; }
     KERNEL_ASSERT(pathname != NULL, "sys_open called with NULL kernel pathname");

     // --- Call VFS (No FD table lock needed yet) ---
     serial_write("[sys_open] Calling vfs_open...\n");
     file_t *vfile = vfs_open(pathname, flags);
     serial_write("[sys_open] vfs_open returned vfile="); serial_print_hex((uintptr_t)vfile); serial_write("\n");
     if (!vfile) { /* ... error handling ... */ return -ENOENT; } // Assume ENOENT for simplicity

     // --- Allocate kernel structures (No FD table lock needed yet) ---
     serial_write("[sys_open] Allocating sys_file_t...\n");
     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) { /* ... error handling, cleanup vfile ... */ vfs_close(vfile); return -ENOMEM; }
     sf->vfs_file = vfile;
     sf->flags = flags;
     serial_write("[sys_open] sys_file_t allocated and populated.\n");

     // --- Allocate FD (This function now handles locking) ---
     serial_write("[sys_open] Allocating file descriptor...\n");
     int fd = allocate_fd_for_process(current_proc, sf);
     if (fd < 0) { // Handle -EMFILE
         SYSFILE_DEBUG_PRINTK(" -> allocate_fd_for_process failed (%d).", fd);
         serial_write("[sys_open] allocate_fd failed. Closing vfile, freeing sf, returning error.\n");
         vfs_close(vfile);
         kfree(sf);
         return fd;
     }

     SYSFILE_DEBUG_PRINTK(" -> Success. Assigned fd %d.", fd);
     serial_write("[sys_open] FD allocated: "); serial_print_hex((uint32_t)fd); serial_write(". Returning fd.\n");
     return fd; // Return the allocated FD
 }

 /**
  * @brief Backend implementation for the read() system call.
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_read with count > 0");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;

     SYSFILE_DEBUG_PRINTK("PID %lu: sys_read(fd=%d, kbuf=%p, count=%zu)", (unsigned long)current_proc->pid, fd, kbuf, count);

     // --- Retrieve sys_file_t safely (Handles locking) ---
     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) return -EBADF;

     // --- Permission Check (Uses sf->flags, no table access needed) ---
     int access_mode = sf->flags & O_ACCMODE;
     if (access_mode != O_RDONLY && access_mode != O_RDWR) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for reading (flags=0x%x, access_mode=0x%x, -EACCES)", fd, sf->flags, access_mode);
         return -EACCES;
     }
     if (count == 0) return 0;

     // --- Call VFS (VFS handles file->lock for offset) ---
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     SYSFILE_DEBUG_PRINTK(" -> vfs_read returned %zd", bytes_read);
     return bytes_read; // Return bytes read or negative error code from VFS
 }

 /**
  * @brief Backend implementation for the write() system call.
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_write with count > 0");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;

     SYSFILE_DEBUG_PRINTK("PID %lu: sys_write(fd=%d, kbuf=%p, count=%zu)", (unsigned long)current_proc->pid, fd, kbuf, count);

     // --- Retrieve sys_file_t safely (Handles locking) ---
     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) return -EBADF;

     // --- Permission Check (Uses sf->flags, no table access needed) ---
     int access_mode = sf->flags & O_ACCMODE;
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for writing (flags=0x%x, access_mode=0x%x, -EACCES)", fd, sf->flags, access_mode);
         return -EACCES;
     }
     if (count == 0) return 0;

     // --- Call VFS (VFS handles file->lock for offset) ---
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     SYSFILE_DEBUG_PRINTK(" -> vfs_write returned %zd", bytes_written);
     return bytes_written; // Return bytes written or negative error code from VFS
 }

 /**
  * @brief Backend implementation for the close() system call.
  * @note Acquires and releases the process's fd_table_lock.
  */
 int sys_close(int fd) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;

     SYSFILE_DEBUG_PRINTK("PID %lu: sys_close(fd=%d)", (unsigned long)current_proc->pid, fd);

     sys_file_t *sf_to_close = NULL; // Store pointer to close outside lock

     // --- Critical Section for FD Table Modification ---
     // === Acquire Lock ===
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);

     // Retrieve the sys_file_t pointer *while holding the lock*
     if (fd < 0 || fd >= MAX_FD) {
         spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid fd %d (out of bounds)", fd);
         return -EBADF;
     }
     sf_to_close = current_proc->fd_table[fd]; // Get pointer

     if (!sf_to_close) {
         // FD was already closed or never opened
         spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid fd %d (not open)", fd);
         return -EBADF; // Bad file descriptor
     }

     // Clear FD table entry FIRST while holding the lock
     current_proc->fd_table[fd] = NULL; // Atomically clear the slot

     // === Release Lock ===
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     // --- End Critical Section ---

     // Now that the FD entry is cleared, we can safely close the underlying file
     // and free the sys_file_t structure without holding the fd_table lock.
     KERNEL_ASSERT(sf_to_close != NULL, "sf_to_close became NULL unexpectedly");
     SYSFILE_DEBUG_PRINTK("   Cleared fd_table[%d]. Closing VFS file %p.", fd, sf_to_close->vfs_file);

     int vfs_ret = vfs_close(sf_to_close->vfs_file); // vfs_close handles freeing vnode and its data
     if (vfs_ret < 0) {
         // Log the error, but POSIX close typically returns 0 even on underlying flush error
         SYSFILE_DEBUG_PRINTK("   Warning: vfs_close for fd %d returned error %d.", fd, vfs_ret);
     } else {
         SYSFILE_DEBUG_PRINTK("   vfs_close succeeded.");
     }

     // Free the sys_file structure itself
     kfree(sf_to_close);

     SYSFILE_DEBUG_PRINTK(" -> Success (fd %d closed).", fd);
     return 0; // POSIX standard is 0 on success
 }

 /**
  * @brief Backend implementation for the lseek() system call.
  */
 off_t sys_lseek(int fd, off_t offset, int whence) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;

     SYSFILE_DEBUG_PRINTK("PID %lu: sys_lseek(fd=%d, offset=%ld, whence=%d)", (unsigned long)current_proc->pid, fd, (long)offset, whence);

     // --- Retrieve sys_file_t safely (Handles locking) ---
     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) return -EBADF;

     // --- Validate whence ---
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid whence %d (-EINVAL)", whence);
         return -EINVAL;
     }

     // --- Call VFS (VFS handles file->lock for offset) ---
     off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
     SYSFILE_DEBUG_PRINTK(" -> vfs_lseek returned %ld", (long)new_pos);

     // vfs_lseek returns the new offset or a negative errno
     return new_pos;
 }
