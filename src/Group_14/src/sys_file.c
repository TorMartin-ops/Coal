/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer (v1.3 - Enhanced Serial Logging)
 * @version 1.3
 *
 * Implements the backend logic for file-related system calls (open, read,
 * write, close, lseek). Interacts with the VFS layer and manages per-process
 * file descriptor tables, now with locking to prevent race conditions.
 * Enhanced with detailed serial logging.
 */

 #include "sys_file.h"
 #include "vfs.h"
 #include "terminal.h"       // Logging (Still useful for higher-level info if needed)
 #include "kmalloc.h"        // kmalloc, kfree
 #include "string.h"         // Kernel string functions
 #include "types.h"          // Core types (ssize_t, off_t, etc.)
 #include "fs_errno.h"       // Filesystem/POSIX error codes (EBADF, EINVAL, EACCES etc.)
 #include "fs_limits.h"      // MAX_FD definition
 #include "process.h"        // pcb_t, get_current_process (includes fd_table_lock now)
 #include "assert.h"         // KERNEL_ASSERT
 #include "debug.h"          // DEBUG_PRINTK macros (can be kept for optional high-level logs)
 #include "serial.h"         // Low-level serial logging for critical paths
 #include "spinlock.h"       // Spinlock functions and type
 
 // Existing debug macro (can be kept or removed)
 #define DEBUG_SYSFILE 0 // Set to 0 if you want ONLY serial logs from this file
 #if DEBUG_SYSFILE
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[SysFile] " fmt "\n", ##__VA_ARGS__)
 #else
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif
 
 // Helper for printing signed integers to serial (basic)
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
 // Internal Helper Functions (Static)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Finds the lowest available file descriptor slot in a process's table.
  * Assigns the sys_file_t structure to that slot.
  */
 static int allocate_fd_for_process(pcb_t *proc, sys_file_t *sf) {
     serial_write("  FNC_ENTER: allocate_fd_for_process\n");
     serial_write("    proc="); serial_print_hex((uintptr_t)proc);
     serial_write(" sf="); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     KERNEL_ASSERT(proc != NULL, "NULL process in allocate_fd");
     KERNEL_ASSERT(sf != NULL, "NULL sys_file in allocate_fd");
 
     int allocated_fd = -EMFILE;
 
     serial_write("    Acquiring fd_table_lock...\n");
     uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);
     serial_write("    Lock acquired.\n");
 
     serial_write("    Searching for free FD slot...\n");
     for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf;
             allocated_fd = fd;
             serial_write("    Found free slot: fd="); serial_print_sdec(fd); serial_write("\n");
             break;
         }
     }
 
     serial_write("    Releasing fd_table_lock...\n");
     spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);
     serial_write("    Lock released.\n");
 
     serial_write("  FNC_EXIT: allocate_fd_for_process ret="); serial_print_sdec(allocated_fd); serial_write("\n");
     return allocated_fd;
 }
 
 /**
  * @brief Retrieves the sys_file_t* for a given FD in a process's table.
  */
 static sys_file_t* get_sys_file(pcb_t *proc, int fd) {
     serial_write("  FNC_ENTER: get_sys_file\n");
     serial_write("    proc="); serial_print_hex((uintptr_t)proc);
     serial_write(" fd="); serial_print_sdec(fd); serial_write("\n");
 
     KERNEL_ASSERT(proc != NULL, "NULL process in get_sys_file");
 
     sys_file_t *sf = NULL;
 
     serial_write("    Acquiring fd_table_lock...\n");
     uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->fd_table_lock);
     serial_write("    Lock acquired.\n");
 
     serial_write("    Checking bounds...\n");
     if (fd < 0 || fd >= MAX_FD) {
         serial_write("    FD out of bounds.\n");
         // sf remains NULL
     } else {
         serial_write("    FD within bounds. Reading fd_table["); serial_print_sdec(fd); serial_write("]...\n");
         sf = proc->fd_table[fd];
         serial_write("    Value read: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
         if (sf == NULL) {
             serial_write("    FD slot is NULL (not open).\n");
         } else {
              serial_write("    FD slot points to sys_file_t. Checking vfs_file...\n");
             KERNEL_ASSERT(sf->vfs_file != NULL, "sys_file_t has NULL vfs_file pointer!");
             serial_write("    vfs_file is valid.\n");
         }
     }
 
     serial_write("    Releasing fd_table_lock...\n");
     spinlock_release_irqrestore(&proc->fd_table_lock, irq_flags);
     serial_write("    Lock released.\n");
 
     serial_write("  FNC_EXIT: get_sys_file ret="); serial_print_hex((uintptr_t)sf); serial_write("\n");
     return sf;
 }
 
 //-----------------------------------------------------------------------------
 // System Call Backend Functions (Called by syscall dispatcher)
 //-----------------------------------------------------------------------------
 
 /**
  * @brief Backend implementation for the open() system call.
  */
 int sys_open(const char *pathname, int flags, int mode) {
     serial_write(" FNC_ENTER: sys_open\n");
     serial_write("   pathname="); serial_print_hex((uintptr_t)pathname); // Print address
     serial_write(" flags=0x"); serial_print_hex((uint32_t)flags);
     serial_write(" mode=0"); serial_print_hex((uint32_t)mode); serial_write("\n"); // mode is octal, print as hex for consistency
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("   ERROR: No current process context.\n");
         serial_write(" FNC_EXIT: sys_open ret=-EFAULT\n");
         return -EFAULT;
     }
     KERNEL_ASSERT(pathname != NULL, "sys_open called with NULL kernel pathname");
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Calling vfs_open...\n");
     file_t *vfile = vfs_open(pathname, flags);
     serial_write("   STEP: vfs_open returned vfile="); serial_print_hex((uintptr_t)vfile); serial_write("\n");
     if (!vfile) {
         serial_write("   ERROR: vfs_open failed.\n");
         serial_write(" FNC_EXIT: sys_open ret=-ENOENT\n");
         return -ENOENT; // Assume ENOENT for simplicity
     }
 
     serial_write("   STEP: Allocating sys_file_t...\n");
     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) {
         serial_write("   ERROR: kmalloc failed for sys_file_t.\n");
         serial_write("   STEP: Cleaning up vfile due to alloc failure...\n");
         vfs_close(vfile);
         serial_write(" FNC_EXIT: sys_open ret=-ENOMEM\n");
         return -ENOMEM;
     }
     sf->vfs_file = vfile;
     sf->flags = flags;
     serial_write("   STEP: sys_file_t allocated and populated at "); serial_print_hex((uintptr_t)sf); serial_write("\n");
 
     serial_write("   STEP: Allocating file descriptor...\n");
     int fd = allocate_fd_for_process(current_proc, sf);
     if (fd < 0) {
         serial_write("   ERROR: allocate_fd_for_process failed.\n");
         serial_write("   STEP: Cleaning up vfile and sf due to FD allocation failure...\n");
         vfs_close(vfile);
         kfree(sf);
         serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(fd); serial_write("\n");
         return fd; // Return -EMFILE
     }
 
     serial_write("   STEP: FD allocated: "); serial_print_sdec(fd); serial_write("\n");
     serial_write(" FNC_EXIT: sys_open ret="); serial_print_sdec(fd); serial_write("\n");
     return fd;
 }
 
 /**
  * @brief Backend implementation for the read() system call.
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     serial_write(" FNC_ENTER: sys_read\n");
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" kbuf="); serial_print_hex((uintptr_t)kbuf);
     serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
 
     if (kbuf == NULL && count != 0) {
         serial_write("   ERROR: NULL kernel buffer with non-zero count.\n");
         serial_write(" FNC_EXIT: sys_read ret=-EFAULT\n");
         return -EFAULT;
     }
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("   ERROR: No current process context.\n");
         serial_write(" FNC_EXIT: sys_read ret=-EFAULT\n");
         return -EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t...\n");
     sys_file_t *sf = get_sys_file(current_proc, fd);
     serial_write("   STEP: get_sys_file returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
     if (!sf) {
         serial_write("   ERROR: Invalid file descriptor.\n");
         serial_write(" FNC_EXIT: sys_read ret=-EBADF\n");
         return -EBADF;
     }
 
     serial_write("   STEP: Checking permissions...\n");
     int access_mode = sf->flags & O_ACCMODE;
     serial_write("     Flags=0x"); serial_print_hex((uint32_t)sf->flags);
     serial_write(" AccessMode=0x"); serial_print_hex((uint32_t)access_mode); serial_write("\n");
     if (access_mode != O_RDONLY && access_mode != O_RDWR) {
         serial_write("   ERROR: File not opened for reading.\n");
         serial_write(" FNC_EXIT: sys_read ret=-EACCES\n");
         return -EACCES;
     }
     if (count == 0) {
         serial_write("   STEP: Count is 0, returning 0.\n");
         serial_write(" FNC_EXIT: sys_read ret=0\n");
         return 0;
     }
     serial_write("   STEP: Permission check passed.\n");
 
     serial_write("   STEP: Calling vfs_read...\n");
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     serial_write("   STEP: vfs_read returned: "); serial_print_sdec(bytes_read); serial_write("\n"); // Print signed result
 
     serial_write(" FNC_EXIT: sys_read ret="); serial_print_sdec(bytes_read); serial_write("\n");
     return bytes_read;
 }
 
 /**
  * @brief Backend implementation for the write() system call.
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     serial_write(" FNC_ENTER: sys_write\n");
     serial_write("   fd="); serial_print_sdec(fd);
     serial_write(" kbuf="); serial_print_hex((uintptr_t)kbuf);
     serial_write(" count="); serial_print_hex((uint32_t)count); serial_write("\n");
 
     if (kbuf == NULL && count != 0) {
         serial_write("   ERROR: NULL kernel buffer with non-zero count.\n");
         serial_write(" FNC_EXIT: sys_write ret=-EFAULT\n");
         return -EFAULT;
     }
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         serial_write("   ERROR: No current process context.\n");
         serial_write(" FNC_EXIT: sys_write ret=-EFAULT\n");
         return -EFAULT;
     }
     serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
     serial_write("   STEP: Retrieving sys_file_t...\n");
     sys_file_t *sf = get_sys_file(current_proc, fd);
     serial_write("   STEP: get_sys_file returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
     if (!sf) {
         serial_write("   ERROR: Invalid file descriptor.\n");
         serial_write(" FNC_EXIT: sys_write ret=-EBADF\n");
         return -EBADF;
     }
 
     serial_write("   STEP: Checking permissions...\n");
     int access_mode = sf->flags & O_ACCMODE;
     serial_write("     Flags=0x"); serial_print_hex((uint32_t)sf->flags);
     serial_write(" AccessMode=0x"); serial_print_hex((uint32_t)access_mode); serial_write("\n");
     if (access_mode != O_WRONLY && access_mode != O_RDWR) {
         serial_write("   ERROR: File not opened for writing.\n");
         serial_write(" FNC_EXIT: sys_write ret=-EACCES\n");
         return -EACCES;
     }
     if (count == 0) {
         serial_write("   STEP: Count is 0, returning 0.\n");
         serial_write(" FNC_EXIT: sys_write ret=0\n");
         return 0;
     }
     serial_write("   STEP: Permission check passed.\n");
 
     serial_write("   STEP: Calling vfs_write...\n");
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     serial_write("   STEP: vfs_write returned: "); serial_print_sdec(bytes_written); serial_write("\n");
 
     serial_write(" FNC_EXIT: sys_write ret="); serial_print_sdec(bytes_written); serial_write("\n");
     return bytes_written;
 }
 
  /**
   * @brief Backend implementation for the close() system call.
   */
  int sys_close(int fd) {
      serial_write(" FNC_ENTER: sys_close\n");
      serial_write("   fd="); serial_print_sdec(fd); serial_write("\n");
 
      pcb_t *current_proc = get_current_process();
      if (!current_proc) {
          serial_write("   ERROR: No current process context.\n");
          serial_write(" FNC_EXIT: sys_close ret=-EFAULT\n");
          return -EFAULT;
      }
      serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
      sys_file_t *sf_to_close = NULL;
 
      serial_write("   STEP: Acquiring fd_table_lock...\n");
      uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
      serial_write("   Lock acquired.\n");
 
      serial_write("   STEP: Checking bounds and reading FD table...\n");
      if (fd < 0 || fd >= MAX_FD) {
          serial_write("   ERROR: FD out of bounds.\n");
          spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
          serial_write(" FNC_EXIT: sys_close ret=-EBADF\n");
          return -EBADF;
      }
      sf_to_close = current_proc->fd_table[fd];
      serial_write("   Value read: "); serial_print_hex((uintptr_t)sf_to_close); serial_write("\n");
 
      if (!sf_to_close) {
          serial_write("   ERROR: FD not open.\n");
          spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
          serial_write(" FNC_EXIT: sys_close ret=-EBADF\n");
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
      if (vfs_ret < 0) {
          serial_write("   WARNING: vfs_close returned error.\n");
          // POSIX close usually ignores errors, but we log it.
      }
 
      serial_write("   STEP: Freeing sys_file_t structure "); serial_print_hex((uintptr_t)sf_to_close); serial_write("...\n");
      kfree(sf_to_close);
      serial_write("   STEP: Freed.\n");
 
      serial_write(" FNC_EXIT: sys_close ret=0\n");
      return 0; // POSIX standard is 0 on success
  }
 
  /**
   * @brief Backend implementation for the lseek() system call.
   */
  off_t sys_lseek(int fd, off_t offset, int whence) {
      serial_write(" FNC_ENTER: sys_lseek\n");
      serial_write("   fd="); serial_print_sdec(fd);
      serial_write(" offset="); serial_print_sdec((int)offset); // Note: Cast to int for simple print
      serial_write(" whence="); serial_print_sdec(whence); serial_write("\n");
 
      pcb_t *current_proc = get_current_process();
      if (!current_proc) {
          serial_write("   ERROR: No current process context.\n");
          serial_write(" FNC_EXIT: sys_lseek ret=-EFAULT\n");
          return -EFAULT;
      }
      serial_write("   Current PID="); serial_print_hex(current_proc->pid); serial_write("\n");
 
      serial_write("   STEP: Retrieving sys_file_t...\n");
      sys_file_t *sf = get_sys_file(current_proc, fd);
      serial_write("   STEP: get_sys_file returned: "); serial_print_hex((uintptr_t)sf); serial_write("\n");
      if (!sf) {
          serial_write("   ERROR: Invalid file descriptor.\n");
          serial_write(" FNC_EXIT: sys_lseek ret=-EBADF\n");
          return -EBADF;
      }
 
      serial_write("   STEP: Validating whence...\n");
      if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
          serial_write("   ERROR: Invalid whence value.\n");
          serial_write(" FNC_EXIT: sys_lseek ret=-EINVAL\n");
          return -EINVAL;
      }
 
      serial_write("   STEP: Calling vfs_lseek...\n");
      off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
      serial_write("   STEP: vfs_lseek returned: "); serial_print_sdec((int)new_pos); serial_write("\n"); // Note: Cast to int
 
      serial_write(" FNC_EXIT: sys_lseek ret="); serial_print_sdec((int)new_pos); serial_write("\n");
      return new_pos;
  }