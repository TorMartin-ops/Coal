/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Implementation Layer
 */

 // (Includes and Debug defines remain the same)
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

 #define DEBUG_SYSFILE 0
 #if DEBUG_SYSFILE
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) DEBUG_PRINTK("[SysFile] " fmt "\n", ##__VA_ARGS__)
 #else
 #define SYSFILE_DEBUG_PRINTK(fmt, ...) ((void)0)
 #endif

 //-----------------------------------------------------------------------------
 // Internal Helper Functions (Static)
 //-----------------------------------------------------------------------------

 // --- allocate_fd_for_process --- (Keep existing implementation)
 // Finds the lowest available FD slot in the process's table.
 static int allocate_fd_for_process(pcb_t *proc, sys_file_t *sf) {
     KERNEL_ASSERT(proc != NULL, "NULL process in allocate_fd");
     KERNEL_ASSERT(sf != NULL, "NULL sys_file in allocate_fd");
     // TODO: SMP Lock needed here
     for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf;
             SYSFILE_DEBUG_PRINTK("PID %lu: Allocated fd %d", proc->pid, fd);
             return fd;
         }
     }
     // TODO: SMP Unlock needed here
     SYSFILE_DEBUG_PRINTK("PID %lu: Failed to allocate FD (-EMFILE)", proc->pid);
     return -EMFILE;
 }

 // --- get_sys_file --- (Keep existing implementation)
 // Retrieves the sys_file_t* for a given FD, checking bounds and validity.
 static sys_file_t* get_sys_file(pcb_t *proc, int fd) {
     KERNEL_ASSERT(proc != NULL, "NULL process in get_sys_file");
     // TODO: SMP Lock needed here (read lock sufficient)
     if (fd < 0 || fd >= MAX_FD) {
         SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (out of bounds)", proc->pid, fd);
         return NULL;
     }
     sys_file_t *sf = proc->fd_table[fd];
     // TODO: SMP Unlock needed here
     if (sf == NULL) {
         SYSFILE_DEBUG_PRINTK("PID %lu: Invalid fd %d (not open)", proc->pid, fd);
         return NULL;
     }
     KERNEL_ASSERT(sf->vfs_file != NULL, "sys_file_t has NULL vfs_file pointer!");
     return sf;
 }

 //-----------------------------------------------------------------------------
 // System Call Backend Functions (Called by syscall dispatcher)
 //-----------------------------------------------------------------------------

 // --- sys_open --- (Keep existing implementation)
 // Handles VFS open, sys_file_t allocation, and FD assignment.
 int sys_open(const char *pathname, int flags, int mode) {
     KERNEL_ASSERT(pathname != NULL, "NULL pathname passed to sys_open");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_open(path='%s', flags=0x%x, mode=0%o)", current_proc->pid, pathname, flags, mode);

     file_t *vfile = vfs_open(pathname, flags); // VFS interaction
     if (!vfile) {
         SYSFILE_DEBUG_PRINTK(" -> vfs_open failed for path '%s'. Returning -ENOENT (assumed).", pathname);
         return -ENOENT; // Or better error from vfs_open if available
     }
     SYSFILE_DEBUG_PRINTK(" -> vfs_open succeeded, vfile=%p", vfile);

     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) {
         SYSFILE_DEBUG_PRINTK(" -> kmalloc failed for sys_file_t. Returning -ENOMEM.");
         vfs_close(vfile);
         return -ENOMEM;
     }
     sf->vfs_file = vfile;
     sf->flags = flags;

     int fd = allocate_fd_for_process(current_proc, sf);
     if (fd < 0) { // -EMFILE
         SYSFILE_DEBUG_PRINTK(" -> allocate_fd_for_process failed (%d).", fd);
         vfs_close(vfile);
         kfree(sf);
         return fd;
     }
     SYSFILE_DEBUG_PRINTK(" -> Success. Assigned fd %d.", fd);
     return fd;
 }

 // --- sys_read --- (Keep existing implementation)
 // Validates FD, checks permissions, calls VFS read.
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_read with count > 0");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_read(fd=%d, kbuf=%p, count=%zu)", current_proc->pid, fd, kbuf, count);

     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) return -EBADF;

     // Check if opened for reading
     if ((sf->flags & O_ACCMODE) != O_RDONLY && (sf->flags & O_ACCMODE) != O_RDWR) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for reading (flags=0x%x, -EBADF)", fd, sf->flags);
         return -EBADF;
     }
     if (count == 0) return 0;

     // Call VFS layer
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     SYSFILE_DEBUG_PRINTK(" -> vfs_read returned %zd", bytes_read);
     return bytes_read;
 }

 // --- sys_write --- (Keep existing implementation)
 // Validates FD, checks permissions, calls VFS write.
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     KERNEL_ASSERT(kbuf != NULL || count == 0, "NULL kernel buffer passed to sys_write with count > 0");
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_write(fd=%d, kbuf=%p, count=%zu)", current_proc->pid, fd, kbuf, count);

     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) return -EBADF;

     // Check if opened for writing
     if ((sf->flags & O_ACCMODE) != O_WRONLY && (sf->flags & O_ACCMODE) != O_RDWR) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: fd %d not opened for writing (flags=0x%x, -EBADF)", fd, sf->flags);
         return -EBADF;
     }
     if (count == 0) return 0;

     // Call VFS layer
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     SYSFILE_DEBUG_PRINTK(" -> vfs_write returned %zd", bytes_written);
     return bytes_written;
 }

 // --- sys_close --- (Keep existing implementation)
 // Validates FD, clears table entry, calls VFS close, frees struct.
 int sys_close(int fd) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_close(fd=%d)", current_proc->pid, fd);

     // TODO: SMP Lock needed here (write lock)
     sys_file_t *sf = get_sys_file(current_proc, fd); // Note: get_sys_file uses read lock, potential upgrade needed or careful locking
     if (!sf) {
         // TODO: SMP Unlock needed here
         return -EBADF;
     }

     // Clear FD table entry FIRST
     KERNEL_ASSERT(current_proc->fd_table[fd] == sf, "FD table inconsistency during close");
     current_proc->fd_table[fd] = NULL;
     // TODO: SMP Unlock needed here

     SYSFILE_DEBUG_PRINTK("   Cleared fd_table[%d]. Closing VFS file %p.", fd, sf->vfs_file);

     // Call VFS close
     int vfs_ret = vfs_close(sf->vfs_file);
     if (vfs_ret < 0) {
         SYSFILE_DEBUG_PRINTK("   Warning: vfs_close for fd %d returned error %d.", fd, vfs_ret);
     } else {
         SYSFILE_DEBUG_PRINTK("   vfs_close succeeded.");
     }

     // Free the sys_file structure
     kfree(sf);

     SYSFILE_DEBUG_PRINTK(" -> Success (fd %d closed).", fd);
     return 0; // POSIX standard is 0 on success
 }

 // --- sys_lseek --- (Keep existing implementation)
 // Validates FD, whence, calls VFS lseek.
 off_t sys_lseek(int fd, off_t offset, int whence) {
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
     SYSFILE_DEBUG_PRINTK("PID %lu: sys_lseek(fd=%d, offset=%ld, whence=%d)", current_proc->pid, fd, (long)offset, whence);

     sys_file_t *sf = get_sys_file(current_proc, fd);
     if (!sf) return -EBADF;

     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         SYSFILE_DEBUG_PRINTK(" -> Failed: Invalid whence %d (-EINVAL)", whence);
         return -EINVAL;
     }

     // Call VFS layer (which should handle non-seekable files like pipes)
     off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
     SYSFILE_DEBUG_PRINTK(" -> vfs_lseek returned %ld", (long)new_pos);
     return new_pos; // Return offset or negative errno from VFS
 }