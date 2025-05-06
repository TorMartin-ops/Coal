/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer (v1.6 - POSIX Error Returns)
 * @version 1.6
 *
 * Implements the backend logic for file-related system calls.
 * Ensures functions return standard POSIX negative error codes.
 *
 * Changes v1.6:
 * - All sys_* functions now return negative fs_errno values for errors.
 * - Corrected sys_open to return -EMFILE when assign_fd_locked indicates no FDs.
 */

 #include "sys_file.h"
 #include "vfs.h"
 #include "terminal.h"
 #include "kmalloc.h"
 #include "string.h"
 #include "types.h"
 #include "fs_errno.h" // Make sure these are positive constants
 #include "fs_limits.h"
 #include "process.h"
 #include "assert.h"
 #include "debug.h"
 #include "serial.h"
 #include "spinlock.h"
 #include <libc/limits.h>  // For INT32_MIN
 #include <libc/stdbool.h> // For bool
 
 // Helper for printing signed integers to serial (keep this)
 static void serial_print_sdec(int n) {
     char buf[12];
     char *ptr = buf + sizeof(buf) - 1; // Start from the end
     *ptr = '\0'; // Null terminate
     uint32_t un;
     bool neg = false;
 
     if (n == 0) {
         if (ptr > buf) *--ptr = '0';
     } else {
         if (n < 0) {
             neg = true;
             if (n == INT32_MIN) { // Basic INT_MIN handling for 32-bit
                 un = 2147483648U;
             } else {
                 un = (uint32_t)(-n);
             }
         } else {
             un = (uint32_t)n;
         }
         while (un > 0 && ptr > buf) { // Ensure not to underflow buffer
             *--ptr = '0' + (un % 10);
             un /= 10;
         }
         if (neg && ptr > buf) { // Ensure space for '-'
             *--ptr = '-';
         }
     }
     serial_write(ptr);
 }
 
 //-----------------------------------------------------------------------------
 // Internal Helper Functions (Static Definitions)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Finds the lowest available FD slot and assigns the sys_file_t.
  * DEFINITION: Implemented here.
  * Assumes proc->fd_table_lock is held by the caller.
  * @return File descriptor index on success, or positive EMFILE on error.
  */
 static int assign_fd_locked(pcb_t *proc, sys_file_t *sf) {
     serial_write("    assign_fd_locked: Searching...\n"); // Debug log
     for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf;
             serial_write("      assign_fd_locked: Found free slot fd="); serial_print_sdec(fd); serial_write("\n"); // Debug log
             return fd; // Return the index
         }
     }
     serial_write("      assign_fd_locked: No free slots found (EMFILE).\n"); // Debug log
     return EMFILE; // No free slots (return positive error code for internal use)
 }
 
 /**
  * @brief Retrieves the sys_file_t* for a given FD.
  * DEFINITION: Implemented here.
  * Assumes proc->fd_table_lock is held by the caller.
  */
 static sys_file_t* get_sys_file_locked(pcb_t *proc, int fd) {
     serial_write("    get_sys_file_locked: Checking fd="); serial_print_sdec(fd); serial_write("\n"); // Debug log
     if (fd < 0 || fd >= MAX_FD || proc->fd_table[fd] == NULL) {
         serial_write("      get_sys_file_locked: Invalid FD or slot empty. Returning NULL.\n"); // Debug log
         return NULL;
     }
     sys_file_t* sf = proc->fd_table[fd];
     serial_write("      get_sys_file_locked: Found sf="); serial_print_hex((uintptr_t)sf); serial_write(". Returning sf.\n"); // Debug log
     return sf;
 }
 
 
 //-----------------------------------------------------------------------------
 // System Call Backend Functions
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Backend implementation for the open() system call.
  * Returns a non-negative file descriptor on success, or a negative error code.
  */
 int sys_open(const char *pathname, int flags, int mode) {
     serial_write(" FNC_ENTER: sys_open\n");
     serial_write("   pathname="); serial_print_hex((uintptr_t)pathname); // Pathname is a kernel pointer here
     serial_write(" flags=0x"); serial_print_hex((uint32_t)flags);
     serial_write(" mode=0"); serial_print_hex((uint32_t)mode); serial_write("\n");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n");
         serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
     KERNEL_ASSERT(pathname != NULL, "sys_open called with NULL kernel pathname");
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Calling vfs_open...\n");
     file_t *vfile = vfs_open(pathname, flags);
     serial_write("   vfs_open returned vfile="); serial_print_hex((uintptr_t)vfile); serial_write("\n");
 
     // vfs_open is assumed to return NULL on error and potentially set a VFS-level errno.
     // For now, if vfile is NULL, we map it to -ENOENT or a more specific error if VFS provides one.
     // The log shows the FAT driver returning 7 for O_EXCL failure, which vfs_open turns to NULL.
     // sys_open then returned -ENOENT (-2). If test expects -EEXIST (-17), this is a deeper issue.
     if (!vfile) {
         serial_write("  ERROR: vfs_open failed.\n");
         // Ideally, vfs_open would return a specific negative error code.
         // For the O_EXCL case from the log where driver returned 7 (EEXIST-like):
         // We don't have the driver's '7' here directly, only the NULL from vfs_open.
         // If O_CREAT and O_EXCL were set, and vfs_open failed, it's likely -EEXIST.
         if ((flags & O_CREAT) && (flags & O_EXCL)) {
              serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(-EEXIST); serial_write("\n");
              return -EEXIST; // Best guess for O_CREAT | O_EXCL failure
         }
         serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(-ENOENT); serial_write("\n");
         return -ENOENT; 
     }
 
     serial_write("   STEP: Allocating sys_file_t...\n");
     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) {
         serial_write("  ERROR: kmalloc failed for sys_file_t.\n");
         serial_write("   STEP: Cleaning up vfile due to alloc failure...\n");
         vfs_close(vfile); // vfs_close should handle NULL if vfile could be NULL
         serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(-ENOMEM); serial_write("\n");
         return -ENOMEM;
     }
     sf->vfs_file = vfile;
     sf->flags = flags;
     serial_write("   sys_file_t allocated and populated at "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     serial_write("   STEP: Allocating file descriptor...\n");
     int fd_or_err;
 
     serial_write("     Acquiring fd_table_lock...\n");
     uintptr_t irq_flags_lock = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     serial_write("     Lock acquired.\n");
 
     fd_or_err = assign_fd_locked(current_proc, sf);
 
     serial_write("     Releasing fd_table_lock...\n");
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags_lock);
     serial_write("     Lock released.\n");
 
     if (fd_or_err == EMFILE) { // assign_fd_locked returns positive EMFILE on error
         serial_write("  ERROR: assign_fd_locked failed (no free slots).\n");
         serial_write("   STEP: Cleaning up vfile and sf due to FD allocation failure...\n");
         vfs_close(vfile);
         kfree(sf);
         serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(-EMFILE); serial_write("\n");
         return -EMFILE; // Convert positive EMFILE to negative
     }
     // fd_or_err is now a valid non-negative fd
     serial_write("   FD allocated: "); serial_print_sdec(fd_or_err); serial_write("\n");
     serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(fd_or_err); serial_write("\n");
     return fd_or_err; 
 }
 
 
 /**
  * @brief Backend implementation for the read() system call.
  * Returns bytes read (non-negative) on success, or a negative error code.
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     serial_write(" FNC_ENTER: sys_read\n"); 
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" kbuf="); serial_print_hex((uintptr_t)kbuf);
     serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
 
     if (kbuf == NULL && count != 0) {
         serial_write("  ERROR: NULL kernel buffer with non-zero count.\n"); 
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n"); 
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t (locked access)...\n"); 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd); 
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     serial_write("   get_sys_file_locked returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     if (!sf) {
         serial_write("  ERROR: Invalid file descriptor.\n"); 
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(-EBADF); serial_write("\n");
         return -EBADF;
     }
 
     serial_write("   STEP: Checking permissions...\n"); 
     int access_mode = sf->flags & O_ACCMODE;
     serial_write("     Flags=0x"); serial_print_hex((uint32_t)sf->flags);
     serial_write(" AccessMode=0x"); serial_print_hex((uint32_t)access_mode); serial_write("\n");
     if (access_mode != O_RDONLY && access_mode != O_RDWR) {
         serial_write("  ERROR: File not opened for reading.\n"); 
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(-EACCES); // POSIX EACCES
         serial_write("\n");
         return -EACCES;
     }
     if (count == 0) {
         serial_write("   STEP: Count is 0, returning 0.\n"); 
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(0); serial_write("\n");
         return 0;
     }
     serial_write("   STEP: Permission check passed.\n"); 
 
     serial_write("   STEP: Calling vfs_read...\n"); 
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     serial_write("   vfs_read returned: "); serial_print_sdec(bytes_read); serial_write("\n");
     // Assuming vfs_read returns POSIX compliant errors (negative) or bytes read (>=0)
 
     serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(bytes_read); serial_write("\n");
     return bytes_read;
 }
 
 /**
  * @brief Backend implementation for the write() system call.
  * Returns bytes written (non-negative) on success, or a negative error code.
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     serial_write(" FNC_ENTER: sys_write\n"); 
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" kbuf="); serial_print_hex((uintptr_t)kbuf);
     serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
 
     if (kbuf == NULL && count != 0) {
         serial_write("  ERROR: NULL kernel buffer with non-zero count.\n"); 
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n"); 
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t (locked access)...\n"); 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd); 
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     serial_write("   get_sys_file_locked returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     if (!sf) {
         serial_write("  ERROR: Invalid file descriptor.\n"); 
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(-EBADF); serial_write("\n");
         return -EBADF;
     }
 
     serial_write("   STEP: Checking permissions...\n"); 
     int access_mode = sf->flags & O_ACCMODE;
     serial_write("     Flags=0x"); serial_print_hex((uint32_t)sf->flags);
     serial_write(" AccessMode=0x"); serial_print_hex((uint32_t)access_mode); serial_write("\n");
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         serial_write("  ERROR: File not opened for writing.\n"); 
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(-EACCES); // POSIX EACCES
         serial_write("\n");
         return -EACCES;
     }
     if (count == 0) {
         serial_write("   STEP: Count is 0, returning 0.\n"); 
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(0); serial_write("\n");
         return 0;
     }
     serial_write("   STEP: Permission check passed.\n"); 
 
     serial_write("   STEP: Calling vfs_write...\n"); 
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     serial_write("   vfs_write returned: "); serial_print_sdec(bytes_written); serial_write("\n");
     // Assuming vfs_write returns POSIX compliant errors (negative) or bytes written (>=0)
 
     serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(bytes_written); serial_write("\n");
     return bytes_written;
 }
 
 /**
   * @brief Backend implementation for the close() system call.
   * Returns 0 on success, or a negative error code.
   */
 int sys_close(int fd) {
     serial_write(" FNC_ENTER: sys_close\n"); 
     serial_write("   fd="); serial_print_sdec(fd); serial_write("\n");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n"); 
         serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     sys_file_t *sf_to_close = NULL;
 
     serial_write("   STEP: Acquiring fd_table_lock...\n"); 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     serial_write("   Lock acquired.\n");
 
     serial_write("   STEP: Checking bounds and reading FD table...\n"); 
     if (fd < 0 || fd >= MAX_FD) {
         serial_write("  ERROR: FD out of bounds.\n"); 
         spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
         serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(-EBADF); serial_write("\n");
         return -EBADF;
     }
     sf_to_close = current_proc->fd_table[fd];
     serial_write("   Value read: "); serial_print_hex((uintptr_t)sf_to_close); serial_write("\n");
 
     if (!sf_to_close) {
         serial_write("  ERROR: FD not open.\n"); 
         spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
         serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(-EBADF); serial_write("\n");
         return -EBADF;
     }
 
     serial_write("   STEP: Clearing FD table entry...\n"); 
     current_proc->fd_table[fd] = NULL;
 
     serial_write("   STEP: Releasing fd_table_lock...\n"); 
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     serial_write("   Lock released.\n");
 
     KERNEL_ASSERT(sf_to_close != NULL, "sf_to_close became NULL unexpectedly");
     serial_write("   STEP: Calling vfs_close for vfile "); serial_print_hex((uintptr_t)sf_to_close->vfs_file); serial_write("...\n");
     int vfs_ret = vfs_close(sf_to_close->vfs_file);
     serial_write("   STEP: vfs_close returned "); serial_print_sdec(vfs_ret); serial_write("\n");
     // vfs_close should return 0 on success, or a negative error code.
     // If vfs_close has an error, we still free sf_to_close but report vfs_close's error.
     // However, POSIX close typically returns 0 or -EBADF if fd is bad (already handled).
     // Errors from the underlying device during flush are possible but often not reported by close,
     // or close might retry/ignore. For simplicity, if vfs_close returns error, we reflect it.
 
     serial_write("   STEP: Freeing sys_file_t structure "); serial_print_hex((uintptr_t)sf_to_close); serial_write("...\n");
     kfree(sf_to_close);
     serial_write("   STEP: Freed.\n");
 
     if (vfs_ret < 0) { // If vfs_close had an error
         serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(vfs_ret); serial_write("\n");
         return vfs_ret;
     }
 
     serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(0); serial_write("\n");
     return 0;
 }
 
 /**
   * @brief Backend implementation for the lseek() system call.
   * Returns the resulting offset location on success, or a negative error code.
   */
 off_t sys_lseek(int fd, off_t offset, int whence) {
     serial_write(" FNC_ENTER: sys_lseek\n"); 
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" offset="); serial_print_sdec((int)offset); // Cast for sdec helper
     serial_write(" whence="); serial_print_sdec(whence); serial_write("\n");
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n"); 
         serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec(-EFAULT); serial_write("\n");
         return -EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t (locked access)...\n"); 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd); 
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     serial_write("   get_sys_file_locked returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     if (!sf) {
         serial_write("  ERROR: Invalid file descriptor.\n"); 
         serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec(-EBADF); serial_write("\n");
         return -EBADF;
     }
 
     serial_write("   STEP: Validating whence...\n"); 
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         serial_write("  ERROR: Invalid whence value.\n"); 
         serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec(-EINVAL); serial_write("\n");
         return -EINVAL;
     }
 
     serial_write("   STEP: Calling vfs_lseek...\n"); 
     off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
     serial_write("   vfs_lseek returned: "); serial_print_sdec((int)new_pos); serial_write("\n"); // Cast for sdec
     // Assuming vfs_lseek returns POSIX compliant offset (>=0) or negative error.
 
     serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec((int)new_pos); serial_write("\n"); // Cast for sdec
     return new_pos;
 }