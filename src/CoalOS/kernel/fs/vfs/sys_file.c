/**
 * @file sys_file.c
 * @brief Kernel-Level File Operation Primitives
 * @author Tor Martin Kohle
 * @version 2.0
 *
 * Implements the kernel-side backend for file-related system calls. This layer
 * interfaces with the VFS to perform operations like open, read, write, close,
 * and lseek. It manages process-specific file descriptor tables and ensures
 * adherence to POSIX error conventions by returning negative errno values on failure.
 * Essential logging for critical paths and errors is maintained via serial output.
 */

 #include <kernel/fs/vfs/sys_file.h>
 #include <kernel/fs/vfs/vfs.h>
 #include <kernel/drivers/display/terminal.h>       // For high-level console output (e.g., STDOUT)
 #include <kernel/memory/kmalloc.h>
 #include <kernel/lib/string.h>
 #include <kernel/core/types.h>
 #include <kernel/fs/vfs/fs_errno.h>       // Defines positive errno constants (EBADF, ENOENT, etc.)
 #include <kernel/fs/vfs/fs_limits.h>      // MAX_FD
 #include <kernel/process/process.h>        // pcb_t, get_current_process
 #include <kernel/lib/assert.h>         // KERNEL_ASSERT
 #include <kernel/drivers/display/serial.h>         // Low-level serial port for debugging
 #include <kernel/sync/spinlock.h>
 #include <libc/limits.h>    // INT32_MIN
 #include <libc/stdbool.h>   // bool
 
 // Conditional debug logging for this module
 #define SYS_FILE_DEBUG_LEVEL 0 // 0: Off, 1: Essential, 2: Verbose
 
 #if SYS_FILE_DEBUG_LEVEL >= 1
     #define SF_LOG(fmt, ...) serial_printf("[SysFile] " fmt "\n", ##__VA_ARGS__) // Assumes serial_printf exists
     // If serial_printf not available, use more basic serial_write combinations
 #else
     #define SF_LOG(fmt, ...) ((void)0)
 #endif
 
 #if SYS_FILE_DEBUG_LEVEL >= 2
     #define SF_DETAILED_LOG(fmt, ...) serial_printf("[SysFile Detailed] " fmt "\n", ##__VA_ARGS__)
 #else
     #define SF_DETAILED_LOG(fmt, ...) ((void)0)
 #endif
 
 
 // Internal helper to find and assign a file descriptor.
 // Assumes proc->fd_table_lock is held.
 static int assign_fd_locked(pcb_t *proc, sys_file_t *sf) {
     // Start from fd=0 since standard I/O descriptors are now properly initialized
    for (int fd = 0; fd < MAX_FD; fd++) {
         if (proc->fd_table[fd] == NULL) {
             proc->fd_table[fd] = sf;
             SF_DETAILED_LOG("Assigned fd %d to sys_file %p", fd, sf);
             return fd;
         }
     }
     SF_LOG("No free file descriptors (EMFILE) for PID %lu", (unsigned long)proc->pid);
     return EMFILE; // Return positive EMFILE for internal error checking
 }
 
 // Internal helper to retrieve a sys_file_t from an fd.
 // Assumes proc->fd_table_lock is held.
 static sys_file_t* get_sys_file_locked(pcb_t *proc, int fd) {
     if (fd < 0 || fd >= MAX_FD || proc->fd_table[fd] == NULL) {
         SF_DETAILED_LOG("Invalid or unassigned fd %d", fd);
         return NULL;
     }
     return proc->fd_table[fd];
 }
 
 /**
  * @brief Implements the sys_open_impl logic.
  * Translates a user-provided path and flags into a VFS file operation,
  * allocating a file descriptor for the calling process.
  * @return File descriptor on success, negative POSIX errno on failure.
  */
 int sys_open(const char *pathname, int flags, int mode) {
     (void)mode; // Mode is often ignored in simple kernels for specific FS types
     SF_LOG("sys_open: path='%s', flags=0x%x", pathname ? pathname : "<NULL>", flags);
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) {
         // This should ideally not happen if syscalls are only from valid processes.
         serial_write("[SysFile CRITICAL] sys_open: No current process!\n");
         return -EFAULT;
     }
     KERNEL_ASSERT(pathname != NULL, "sys_open: NULL kernel pathname");
 
     file_t *vfs_file = vfs_open(pathname, flags);
     if (!vfs_file) {
         // vfs_open is expected to return NULL and VFS layer might set a more specific
         // thread-local errno, or we infer it. For simplicity, if O_CREAT|O_EXCL
         // failed, it's likely -EEXIST. Otherwise, -ENOENT is a common fallback.
         // A more robust VFS would return specific negative error codes.
         SF_LOG("sys_open: vfs_open failed for path '%s'", pathname);
         if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
         return -ENOENT; // Common error if file not found and O_CREAT not set or failed.
     }
 
     sys_file_t *sf = (sys_file_t *)kmalloc(sizeof(sys_file_t));
     if (!sf) {
         vfs_close(vfs_file); // Clean up allocated VFS file
         SF_LOG("sys_open: kmalloc for sys_file_t failed for path '%s'", pathname);
         return -ENOMEM;
     }
     sf->vfs_file = vfs_file;
     sf->flags = flags;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     int fd_or_err = assign_fd_locked(current_proc, sf);
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
 
     if (fd_or_err == EMFILE) { // assign_fd_locked returns positive EMFILE
         vfs_close(vfs_file);
         kfree(sf);
         SF_LOG("sys_open: No free FDs (EMFILE) for path '%s'", pathname);
         return -EMFILE; // Convert to negative errno
     }
 
     SF_LOG("sys_open: Success. Path '%s' -> fd %d", pathname, fd_or_err);
     return fd_or_err; // This is the non-negative file descriptor
 }
 
 /**
  * @brief Implements the sys_read_impl logic.
  * Reads from the file associated with the descriptor into a kernel buffer.
  * @return Number of bytes read, or negative POSIX errno on failure.
  */
 ssize_t sys_read(int fd, void *kbuf, size_t count) {
     SF_LOG("sys_read: fd=%d, count=%lu", fd, (unsigned long)count);
     if (kbuf == NULL && count != 0) return -EFAULT;
     if (count == 0) return 0;
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd);
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
 
     if (!sf) return -EBADF;
 
     // Check if file was opened with read permission
     if (!((sf->flags & O_ACCMODE) == O_RDONLY || (sf->flags & O_ACCMODE) == O_RDWR)) {
         SF_LOG("sys_read: fd %d not opened for reading (flags 0x%x)", fd, sf->flags);
         return -EACCES;
     }
 
     ssize_t bytes_read = vfs_read(sf->vfs_file, kbuf, count);
     SF_LOG("sys_read: fd %d, vfs_read returned %d", fd, (int)bytes_read);
     return bytes_read; // vfs_read returns bytes read (>=0) or negative FS_ERR_*
 }
 
 /**
  * @brief Implements the sys_write_impl logic.
  * Writes from a kernel buffer to the file associated with the descriptor.
  * @return Number of bytes written, or negative POSIX errno on failure.
  */
 ssize_t sys_write(int fd, const void *kbuf, size_t count) {
     SF_LOG("sys_write: fd=%d, count=%lu", fd, (unsigned long)count);
     if (kbuf == NULL && count != 0) return -EFAULT;
     if (count == 0) return 0;
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd);
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
 
     if (!sf) return -EBADF;
 
     // Check if file was opened with write permission
     if (!((sf->flags & O_ACCMODE) == O_WRONLY || (sf->flags & O_ACCMODE) == O_RDWR)) {
         SF_LOG("sys_write: fd %d not opened for writing (flags 0x%x)", fd, sf->flags);
         return -EACCES;
     }
 
     ssize_t bytes_written = vfs_write(sf->vfs_file, kbuf, count);
     SF_LOG("sys_write: fd %d, vfs_write returned %d", fd, (int)bytes_written);
     return bytes_written; // vfs_write returns bytes written (>=0) or negative FS_ERR_*
 }
 
 /**
  * @brief Implements the sys_close_impl logic.
  * Closes a file descriptor, releasing associated VFS resources.
  * @return 0 on success, negative POSIX errno on failure.
  */
 int sys_close(int fd) {
     SF_LOG("sys_close: fd=%d", fd);
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     sys_file_t *sf_to_close = NULL;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     if (fd < 0 || fd >= MAX_FD) {
         spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
         return -EBADF;
     }
     sf_to_close = current_proc->fd_table[fd];
     if (!sf_to_close) {
         spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
         return -EBADF;
     }
     current_proc->fd_table[fd] = NULL; // Clear FD entry under lock
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
 
     KERNEL_ASSERT(sf_to_close != NULL, "sf_to_close became NULL post-lock");
 
     int vfs_ret = vfs_close(sf_to_close->vfs_file); // vfs_close handles its own internal locking.
     kfree(sf_to_close);
 
     SF_LOG("sys_close: fd %d, vfs_close returned %d", fd, vfs_ret);
     // POSIX close typically returns 0 on success or -EBADF.
     // Errors from underlying device flush in VFS close might return other errnos.
     return vfs_ret; // Propagate VFS error or success.
 }
 
 /**
  * @brief Implements the sys_lseek_impl logic.
  * Repositions the read/write file offset.
  * @return Resulting offset location from the beginning of the file on success,
  * or a negative POSIX errno on failure.
  */
 off_t sys_lseek(int fd, off_t offset, int whence) {
     SF_LOG("sys_lseek: fd=%d, offset=%ld, whence=%d", fd, (long)offset, whence);
 
     pcb_t *current_proc = get_current_process();
     if (!current_proc) return -EFAULT;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&current_proc->fd_table_lock);
     sys_file_t *sf = get_sys_file_locked(current_proc, fd);
     spinlock_release_irqrestore(&current_proc->fd_table_lock, irq_flags);
 
     if (!sf) return -EBADF;
 
     // Basic whence validation (VFS layer also validates)
     if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
         return -EINVAL;
     }
 
     off_t new_pos = vfs_lseek(sf->vfs_file, offset, whence);
     SF_LOG("sys_lseek: fd %d, vfs_lseek returned %ld", fd, (long)new_pos);
     return new_pos; // vfs_lseek returns new offset (>=0) or negative FS_ERR_*
 }

//----------------------------------------------------------------------------
// VFS Performance Optimizations - Read-ahead Caching
//----------------------------------------------------------------------------

#define READAHEAD_SIZE (8 * 1024)  // 8KB read-ahead

typedef struct readahead_buffer {
    sys_file_t *file;
    off_t buffer_start_offset;
    size_t buffer_valid_size;
    uint32_t last_access;
    char data[READAHEAD_SIZE];
} readahead_buffer_t;

static struct {
    readahead_buffer_t buffers[4];
    spinlock_t lock;
    uint32_t hits;
    uint32_t misses;
    bool initialized;
} g_readahead_cache = {0};

void vfs_performance_init(void) {
    if (g_readahead_cache.initialized) return;
    
    spinlock_init(&g_readahead_cache.lock);
    for (int i = 0; i < 4; i++) {
        g_readahead_cache.buffers[i].file = NULL;
        g_readahead_cache.buffers[i].buffer_valid_size = 0;
    }
    g_readahead_cache.initialized = true;
    SF_LOG("VFS read-ahead cache initialized");
}

// Try to satisfy read from cache - returns bytes read or 0 for cache miss
static ssize_t try_readahead_cache(sys_file_t *sf, void *buf, size_t count, off_t offset) {
    if (!sf || !buf || count == 0 || !g_readahead_cache.initialized) return 0;
    
    uintptr_t flags = spinlock_acquire_irqsave(&g_readahead_cache.lock);
    
    for (int i = 0; i < 4; i++) {
        readahead_buffer_t *rb = &g_readahead_cache.buffers[i];
        
        if (rb->file == sf && 
            offset >= rb->buffer_start_offset && 
            offset + count <= rb->buffer_start_offset + rb->buffer_valid_size) {
            
            size_t buffer_offset = offset - rb->buffer_start_offset;
            memcpy(buf, &rb->data[buffer_offset], count);
            g_readahead_cache.hits++;
            rb->last_access++; // Simple LRU counter
            
            spinlock_release_irqrestore(&g_readahead_cache.lock, flags);
            return count;
        }
    }
    
    g_readahead_cache.misses++;
    spinlock_release_irqrestore(&g_readahead_cache.lock, flags);
    return 0; // Cache miss
}

// Update cache after successful read for future read-ahead
static void update_readahead_cache(sys_file_t *sf, off_t offset, const void *data, size_t size) {
    if (!sf || !data || size == 0 || size > READAHEAD_SIZE || !g_readahead_cache.initialized) return;
    
    // Only cache for reasonably sized reads that might benefit from read-ahead
    if (size < 512) return;
    
    uintptr_t flags = spinlock_acquire_irqsave(&g_readahead_cache.lock);
    
    // Find LRU buffer (lowest last_access)
    readahead_buffer_t *target = &g_readahead_cache.buffers[0];
    for (int i = 1; i < 4; i++) {
        if (g_readahead_cache.buffers[i].last_access < target->last_access) {
            target = &g_readahead_cache.buffers[i];
        }
    }
    
    target->file = sf;
    target->buffer_start_offset = offset;
    target->buffer_valid_size = size;
    target->last_access++; 
    memcpy(target->data, data, size);
    
    spinlock_release_irqrestore(&g_readahead_cache.lock, flags);
}

// Get VFS cache statistics for performance monitoring
void vfs_get_cache_stats(uint32_t *readahead_hits, uint32_t *readahead_misses) {
    if (!g_readahead_cache.initialized) {
        if (readahead_hits) *readahead_hits = 0;
        if (readahead_misses) *readahead_misses = 0;
        return;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&g_readahead_cache.lock);
    if (readahead_hits) *readahead_hits = g_readahead_cache.hits;
    if (readahead_misses) *readahead_misses = g_readahead_cache.misses;
    spinlock_release_irqrestore(&g_readahead_cache.lock, flags);
}
