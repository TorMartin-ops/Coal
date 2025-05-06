/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer (v1.5 - Linker Fix)
 * @version 1.5
 *
 * Implements the backend logic for file-related system calls.
 * Corrected sys_open return value and removed undefined logging function calls.
 * Ensures helper functions are defined locally.
 *
 * Changes v1.5:
 * - Removed calls to undefined serial_log_* helper functions.
 * - Ensured assign_fd_locked and get_sys_file_locked are defined static locally.
 */

 #include "sys_file.h"
 #include "vfs.h"
 #include "terminal.h"
 #include "kmalloc.h"
 #include "string.h"
 #include "types.h"
 #include "fs_errno.h"
 #include "fs_limits.h"
 #include "process.h"
 #include "assert.h"
 #include "debug.h"
 #include "serial.h"
 #include "spinlock.h"
 
 // Helper for printing signed integers to serial (keep this)
 static void serial_print_sdec(int n) {
     char buf[12];
     char *ptr = buf + 11;
     *ptr = '\0';
     bool neg = false;
     uint32_t un;
 
     if (n == 0) {
         *--ptr = '0';
     } else {
         if (n < 0) {
             neg = true;
             if (n == -2147483648) { // Basic INT_MIN handling for 32-bit
                  un = 2147483648U;
             } else {
                 un = (uint32_t)(-n);
             }
         } else {
             un = (uint32_t)n;
         }
         while (un > 0 && ptr > buf) {
             *--ptr = '0' + (un % 10);
             un /= 10;
         }
         if (neg && ptr > buf) {
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
     return EMFILE; // No free slots
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
 // System Call Backend Functions (Removed serial_log_* calls)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Backend implementation for the open() system call.
  * CORRECTED: Ensures the integer file descriptor index is returned.
  * Calls locally defined assign_fd_locked.
  */
  int sys_open(const char *pathname, int flags, int mode) {
    // Keep direct serial writes for tracing
    serial_write(" FNC_ENTER: sys_open\n");
    serial_write("   pathname="); serial_print_hex((uintptr_t)pathname);
    serial_write(" flags=0x"); serial_print_hex((uint32_t)flags);
    serial_write(" mode=0"); serial_print_hex((uint32_t)mode); serial_write("\n");

    pcb_t *current_proc = get_current_process();
    if (!current_proc) {
        serial_write("  ERROR: No current process context.\n");
        serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(EFAULT); serial_write("\n");
        return EFAULT;
    }
    KERNEL_ASSERT(pathname != NULL, "sys_open called with NULL kernel pathname");
    serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");

    serial_write("   STEP: Calling vfs_open...\n");
    file_t *vfile = vfs_open(pathname, flags);
    serial_write("   vfs_open returned vfile="); serial_print_hex((uintptr_t)vfile); serial_write("\n");

    if (!vfile) {
        serial_write("  ERROR: vfs_open failed.\n");
        serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(-ENOENT); serial_write("\n");
        return -ENOENT;
    }

    serial_write("   STEP: Allocating sys_file_t...\n");
    sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
    if (!sf) {
        serial_write("  ERROR: kmalloc failed for sys_file_t.\n");
        serial_write("   STEP: Cleaning up vfile due to alloc failure...\n");
        vfs_close(vfile);
        serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(ENOMEM); serial_write("\n");
        return ENOMEM;
    }
    sf->vfs_file = vfile;
    sf->flags = flags;
    serial_write("   sys_file_t allocated and populated at "); serial_print_hex((uintptr_t)sf); serial_write("\n");

    serial_write("   STEP: Allocating file descriptor...\n");
    int fd = EMFILE;

    serial_write("    Acquiring fd_table_lock...\n");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
    serial_write("    Lock acquired.\n");

    // *** CORE FIX: Call locally defined locked helper ***
    fd = assign_fd_locked(current_proc, sf);

    serial_write("    Releasing fd_table_lock...\n");
    spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
    serial_write("    Lock released.\n");

    if (fd < 0) {
        serial_write("  ERROR: assign_fd_locked failed (no free slots).\n");
        serial_write("   STEP: Cleaning up vfile and sf due to FD allocation failure...\n");
        vfs_close(vfile);
        kfree(sf);
        serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(fd); serial_write("\n");
        return fd;
    }

    serial_write("   FD allocated: "); serial_print_sdec(fd); serial_write("\n");
    serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(fd); serial_write("\n");
    return fd; // Return the integer index
}

 
 /**
  * @brief Backend implementation for the read() system call.
  * Uses locally defined locked helper for getting sys_file_t.
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     serial_write(" FNC_ENTER: sys_read\n"); // Use direct serial_write
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" kbuf="); serial_print_hex((uintptr_t)kbuf);
     serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
 
     if (kbuf == NULL && count != 0) {
         serial_write("  ERROR: NULL kernel buffer with non-zero count.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(EFAULT); serial_write("\n");
         return EFAULT;
     }
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(EFAULT); serial_write("\n");
         return EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t (locked access)...\n"); // Use direct serial_write
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd); // Use the defined static helper
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     serial_write("   get_sys_file_locked returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     if (!sf) {
         serial_write("  ERROR: Invalid file descriptor.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(EBADF); serial_write("\n");
         return EBADF;
     }
 
     serial_write("   STEP: Checking permissions...\n"); // Use direct serial_write
     int access_mode = sf->flags & O_ACCMODE;
     serial_write("     Flags=0x"); serial_print_hex((uint32_t)sf->flags);
     serial_write(" AccessMode=0x"); serial_print_hex((uint32_t)access_mode); serial_write("\n");
     if (access_mode != O_RDONLY && access_mode != O_RDWR) {
         serial_write("  ERROR: File not opened for reading.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(EACCES); serial_write("\n");
         return EACCES;
     }
     if (count == 0) {
         serial_write("   STEP: Count is 0, returning 0.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(0); serial_write("\n");
         return 0;
     }
     serial_write("   STEP: Permission check passed.\n"); // Use direct serial_write
 
     serial_write("   STEP: Calling vfs_read...\n"); // Use direct serial_write
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     serial_write("   vfs_read returned: "); serial_print_sdec(bytes_read); serial_write("\n");
 
     serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(bytes_read); serial_write("\n");
     return bytes_read;
 }
 
 /**
  * @brief Backend implementation for the write() system call.
  * Uses locally defined locked helper for getting sys_file_t.
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     serial_write(" FNC_ENTER: sys_write\n"); // Use direct serial_write
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" kbuf="); serial_print_hex((uintptr_t)kbuf);
     serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
 
     if (kbuf == NULL && count != 0) {
         serial_write("  ERROR: NULL kernel buffer with non-zero count.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(EFAULT); serial_write("\n");
         return EFAULT;
     }
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("  ERROR: No current process context.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(EFAULT); serial_write("\n");
         return EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t (locked access)...\n"); // Use direct serial_write
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd); // Use the defined static helper
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
     serial_write("   get_sys_file_locked returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     if (!sf) {
         serial_write("  ERROR: Invalid file descriptor.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(EBADF); serial_write("\n");
         return EBADF;
     }
 
     serial_write("   STEP: Checking permissions...\n"); // Use direct serial_write
     int access_mode = sf->flags & O_ACCMODE;
     serial_write("     Flags=0x"); serial_print_hex((uint32_t)sf->flags);
     serial_write(" AccessMode=0x"); serial_print_hex((uint32_t)access_mode); serial_write("\n");
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         serial_write("  ERROR: File not opened for writing.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(EACCES); serial_write("\n");
         return EACCES;
     }
     if (count == 0) {
         serial_write("   STEP: Count is 0, returning 0.\n"); // Use direct serial_write
         serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(0); serial_write("\n");
         return 0;
     }
     serial_write("   STEP: Permission check passed.\n"); // Use direct serial_write
 
     serial_write("   STEP: Calling vfs_write...\n"); // Use direct serial_write
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     serial_write("   vfs_write returned: "); serial_print_sdec(bytes_written); serial_write("\n");
 
     serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(bytes_written); serial_write("\n");
     return bytes_written;
 }
 
  /**
   * @brief Backend implementation for the close() system call.
   */
  int sys_close(int fd) {
      serial_write(" FNC_ENTER: sys_close\n"); // Use direct serial_write
      serial_write("   fd="); serial_print_sdec(fd); serial_write("\n");
 
      pcb_t *current_proc = get_current_process();
      if (!current_proc) {
          serial_write("  ERROR: No current process context.\n"); // Use direct serial_write
          serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(EFAULT); serial_write("\n");
          return EFAULT;
      }
      serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
      sys_file_t *sf_to_close = NULL;
 
      serial_write("   STEP: Acquiring fd_table_lock...\n"); // Use direct serial_write
      uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
      serial_write("   Lock acquired.\n");
 
      serial_write("   STEP: Checking bounds and reading FD table...\n"); // Use direct serial_write
      if (fd < 0 || fd >= MAX_FD) {
          serial_write("  ERROR: FD out of bounds.\n"); // Use direct serial_write
          spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
          serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(EBADF); serial_write("\n");
          return EBADF;
      }
      sf_to_close = current_proc->fd_table[fd];
      serial_write("   Value read: "); serial_print_hex((uintptr_t)sf_to_close); serial_write("\n");
 
      if (!sf_to_close) {
          serial_write("  ERROR: FD not open.\n"); // Use direct serial_write
          spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
          serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(EBADF); serial_write("\n");
          return EBADF;
      }
 
      serial_write("   STEP: Clearing FD table entry...\n"); // Use direct serial_write
      current_proc->fd_table[fd] = NULL;
 
      serial_write("   STEP: Releasing fd_table_lock...\n"); // Use direct serial_write
      spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
      serial_write("   Lock released.\n");
 
      KERNEL_ASSERT(sf_to_close != NULL, "sf_to_close became NULL unexpectedly");
      serial_write("   STEP: Calling vfs_close for vfile "); serial_print_hex((uintptr_t)sf_to_close->vfs_file); serial_write("...\n");
      int vfs_ret = vfs_close(sf_to_close->vfs_file);
      serial_write("   STEP: vfs_close returned "); serial_print_sdec(vfs_ret); serial_write("\n");
      if (vfs_ret < 0) {
          serial_write("   WARNING: vfs_close returned error.\n");
      }
 
      serial_write("   STEP: Freeing sys_file_t structure "); serial_print_hex((uintptr_t)sf_to_close); serial_write("...\n");
      kfree(sf_to_close);
      serial_write("   STEP: Freed.\n");
 
      serial_write(" FNC_EXIT: sys_close ret="); serial_print_sdec(0); serial_write("\n");
      return 0;
  }
 
   /**
    * @brief Backend implementation for the lseek() system call.
    * Uses locally defined locked helper for getting sys_file_t.
    */
   off_t sys_lseek(int fd, off_t offset, int whence) {
       serial_write(" FNC_ENTER: sys_lseek\n"); // Use direct serial_write
       serial_write("   fd="); serial_print_sdec(fd);
       serial_write(" offset="); serial_print_sdec((int)offset);
       serial_write(" whence="); serial_print_sdec(whence); serial_write("\n");
 
       pcb_t *current_proc = get_current_process();
       if (!current_proc) {
           serial_write("  ERROR: No current process context.\n"); // Use direct serial_write
           serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec(EFAULT); serial_write("\n");
           return EFAULT;
       }
       serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
       serial_write("   STEP: Retrieving sys_file_t (locked access)...\n"); // Use direct serial_write
       uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
       sys_file_t *sf = get_sys_file_locked(current_proc, fd); // Use the defined static helper
       spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
       serial_write("   get_sys_file_locked returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
       if (!sf) {
           serial_write("  ERROR: Invalid file descriptor.\n"); // Use direct serial_write
           serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec(EBADF); serial_write("\n");
           return EBADF;
       }
 
       serial_write("   STEP: Validating whence...\n"); // Use direct serial_write
       if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
           serial_write("  ERROR: Invalid whence value.\n"); // Use direct serial_write
           serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec(-EINVAL); serial_write("\n");
           return -EINVAL;
       }
 
       serial_write("   STEP: Calling vfs_lseek...\n"); // Use direct serial_write
       off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
       serial_write("   vfs_lseek returned: "); serial_print_sdec((int)new_pos); serial_write("\n");
 
       serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec((int)new_pos); serial_write("\n");
       return new_pos;
   }