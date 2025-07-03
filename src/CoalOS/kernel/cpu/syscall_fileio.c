/**
 * @file syscall_fileio.c
 * @brief File I/O System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles file input/output system calls including read, write, open,
 * close, lseek, and file descriptor duplication. Focuses purely on file I/O
 * operations while delegating to specialized modules for pipes and terminal.
 */

//============================================================================
// Includes
//============================================================================
#include "syscall_fileio.h"
#include "syscall_utils.h"
#include "syscall_security.h"
#include <kernel/process/process.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/memory/uaccess.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>

//============================================================================
// Configuration
//============================================================================
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef MAX_RW_CHUNK_SIZE
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define MAX_RW_CHUNK_SIZE PAGE_SIZE
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

//============================================================================
// Forward Declarations for External Functions
//============================================================================
// These will be implemented in other modules
extern ssize_t pipe_read_operation(vnode_t *vnode, void *buffer, size_t count, off_t offset);
extern ssize_t pipe_write_operation(vnode_t *vnode, const void *buffer, size_t count, off_t offset);
extern int pipe_close_operation(vnode_t *vnode, bool is_write_end);

//============================================================================
// File I/O System Call Implementation
//============================================================================

int32_t sys_read_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs)
{
    (void)regs;
    int fd = (int)fd_arg;
    userptr_t user_buf = (userptr_t)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t total_read = 0;
    char* kbuf = NULL;

    if ((ssize_t)count < 0) return -EINVAL;
    if (count == 0) return 0;
    
    // Use enhanced security validation
    if (!syscall_validate_buffer(user_buf, count, true)) {
        serial_printf("[sys_read] Buffer validation failed: ptr=0x%x, count=%zu\n", 
                     (uintptr_t)user_buf, count);
        return -EFAULT;
    }

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) return -ENOMEM;

    while (total_read < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_read);
        KERNEL_ASSERT(current_chunk_size > 0, "Read chunk size zero");

        ssize_t bytes_read_this_chunk;
        
        // Special case for STDIN_FILENO - delegate to terminal module
        if (fd == STDIN_FILENO) {
            bytes_read_this_chunk = terminal_read_line_blocking(kbuf, current_chunk_size);
        }
        // Check if this is a pipe file descriptor
        else {
            pcb_t *current_process = get_current_process();
            if (current_process && fd >= 0 && fd < MAX_FD && current_process->fd_table[fd]) {
                sys_file_t *sf = current_process->fd_table[fd];
                if (sf && sf->vfs_file && sf->vfs_file->vnode && 
                    sf->vfs_file->vnode->fs_driver == NULL && sf->vfs_file->vnode->data != NULL) {
                    // This is a pipe - delegate to pipe module
                    bytes_read_this_chunk = pipe_read_operation(sf->vfs_file->vnode, kbuf, current_chunk_size, 0);
                } else {
                    // Regular file - use VFS layer
                    bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
                }
            } else {
                // Invalid fd or process - use existing VFS path which will return appropriate error
                bytes_read_this_chunk = sys_read(fd, kbuf, current_chunk_size);
            }
        }

        if (bytes_read_this_chunk < 0) {
            if (total_read > 0) break;
            total_read = bytes_read_this_chunk;
            break;
        }
        if (bytes_read_this_chunk == 0) break;

        if (copy_to_user((userptr_t)((char*)user_buf + total_read), 
                        (const_kernelptr_t)kbuf, (size_t)bytes_read_this_chunk) != 0) {
            if (total_read > 0) break;
            total_read = -EFAULT;
            break;
        }
        total_read += bytes_read_this_chunk;
        if ((size_t)bytes_read_this_chunk < current_chunk_size) break;
    }
    
    if (kbuf) kfree(kbuf);
    return total_read;
}

int32_t sys_write_impl(uint32_t fd_arg, uint32_t user_buf_ptr, uint32_t count_arg, isr_frame_t *regs)
{
    (void)regs;
    int fd = (int)fd_arg;
    const_userptr_t user_buf = (const_userptr_t)user_buf_ptr;
    size_t count = (size_t)count_arg;
    ssize_t total_written = 0;
    char* kbuf = NULL;

    if ((ssize_t)count < 0) return -EINVAL;
    if (count == 0) return 0;
    
    // Use enhanced security validation
    if (!syscall_validate_buffer((userptr_t)user_buf, count, false)) {
        serial_printf("[sys_write] Buffer validation failed: ptr=0x%x, count=%zu\n", 
                     (uintptr_t)user_buf, count);
        return -EFAULT;
    }

    size_t chunk_alloc_size = MIN(MAX_RW_CHUNK_SIZE, count);
    kbuf = kmalloc(chunk_alloc_size);
    if (!kbuf) return -ENOMEM;

    while (total_written < (ssize_t)count) {
        size_t current_chunk_size = MIN(chunk_alloc_size, count - (size_t)total_written);
        KERNEL_ASSERT(current_chunk_size > 0, "Write chunk size zero");

        size_t not_copied_from_user = copy_from_user((kernelptr_t)kbuf, 
                                                   (const_userptr_t)((const char*)user_buf + total_written), 
                                                   current_chunk_size);
        size_t copied_this_chunk_from_user = current_chunk_size - not_copied_from_user;

        if (copied_this_chunk_from_user > 0) {
            ssize_t bytes_written_this_chunk;
            
            // Special case for stdout/stderr - delegate to terminal module
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                terminal_write_bytes(kbuf, copied_this_chunk_from_user);
                bytes_written_this_chunk = copied_this_chunk_from_user;
            } else {
                // Check if this is a pipe file descriptor
                pcb_t *current_process = get_current_process();
                if (current_process && fd >= 0 && fd < MAX_FD && current_process->fd_table[fd]) {
                    sys_file_t *sf = current_process->fd_table[fd];
                    if (sf && sf->vfs_file && sf->vfs_file->vnode && 
                        sf->vfs_file->vnode->fs_driver == NULL && sf->vfs_file->vnode->data != NULL) {
                        // This is a pipe - delegate to pipe module
                        bytes_written_this_chunk = pipe_write_operation(sf->vfs_file->vnode, kbuf, copied_this_chunk_from_user, 0);
                    } else {
                        // Regular file - use VFS layer
                        bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk_from_user);
                    }
                } else {
                    // Invalid fd or process - use existing VFS path which will return appropriate error
                    bytes_written_this_chunk = sys_write(fd, kbuf, copied_this_chunk_from_user);
                }
            }

            if (bytes_written_this_chunk < 0) {
                if (total_written > 0) break;
                total_written = bytes_written_this_chunk;
                break;
            }
            total_written += bytes_written_this_chunk;
            if ((size_t)bytes_written_this_chunk < copied_this_chunk_from_user) break;
        }

        if (not_copied_from_user > 0) {
             if (total_written > 0) break;
             total_written = -EFAULT;
             break;
        }
        if (copied_this_chunk_from_user == 0 && not_copied_from_user == 0 && current_chunk_size > 0) {
            if (total_written > 0) break;
            total_written = -EFAULT;
            break;
        }
    }
    
    if (kbuf) kfree(kbuf);
    return total_written;
}

int32_t sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags_arg, uint32_t mode_arg, isr_frame_t *regs)
{
    (void)regs;
    const_userptr_t user_pathname = (const_userptr_t)user_pathname_ptr;
    int flags = (int)flags_arg;
    int mode = (int)mode_arg;
    char k_pathname[SYSCALL_MAX_PATH_LEN];

    // Use enhanced path copying with security checks
    int copy_err = syscall_copy_path_from_user(user_pathname, k_pathname, sizeof(k_pathname));
    if (copy_err != 0) {
        serial_printf("[sys_open] Failed to copy pathname: error %d\n", copy_err);
        return copy_err;
    }

    serial_printf("[sys_open] Opening file: '%s' with flags=0x%x, mode=0%o\n", 
                  k_pathname, flags, mode);
    return sys_open(k_pathname, flags, mode);
}

int32_t sys_close_impl(uint32_t fd_arg, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg2; (void)arg3; (void)regs;
    int fd = (int)fd_arg;
    
    // Check if this is a pipe file descriptor
    pcb_t *current_process = get_current_process();
    if (current_process && fd >= 0 && fd < MAX_FD && current_process->fd_table[fd]) {
        sys_file_t *sf = current_process->fd_table[fd];
        if (sf && sf->vfs_file && sf->vfs_file->vnode && 
            sf->vfs_file->vnode->fs_driver == NULL && sf->vfs_file->vnode->data != NULL) {
            // This is a pipe - delegate to pipe module
            vnode_t *vnode = sf->vfs_file->vnode;
            bool is_write_end = (sf->flags & O_WRONLY) != 0;
            
            // Clear the file descriptor from the table first
            uintptr_t irq_flags = spinlock_acquire_irqsave(&current_process->fd_table_lock);
            current_process->fd_table[fd] = NULL;
            spinlock_release_irqrestore(&current_process->fd_table_lock, irq_flags);
            
            // Call pipe close operation
            int result = pipe_close_operation(vnode, is_write_end);
            
            // Free the sys_file structure
            if (sf->vfs_file) {
                kfree(sf->vfs_file);
            }
            kfree(sf);
            
            return result;
        }
    }
    
    // Not a pipe - use regular file close via VFS
    return sys_close(fd);
}

int32_t sys_lseek_impl(uint32_t fd_arg, uint32_t offset_arg, uint32_t whence_arg, isr_frame_t *regs)
{
    (void)regs;
    int fd = (int)fd_arg;
    off_t offset = (off_t)(int32_t)offset_arg; // Cast to signed 32-bit then to off_t
    int whence = (int)whence_arg;
    
    return sys_lseek(fd, offset, whence);
}

int32_t sys_dup2_impl(uint32_t oldfd, uint32_t newfd, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) return -ESRCH;
    
    // Basic validation
    if ((int)oldfd < 0 || (int)oldfd >= MAX_FD || (int)newfd < 0 || (int)newfd >= MAX_FD) {
        return -EBADF;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&current->fd_table_lock);
    
    sys_file_t *old_sf = current->fd_table[oldfd];
    if (!old_sf) {
        spinlock_release_irqrestore(&current->fd_table_lock, flags);
        return -EBADF;
    }
    
    // If newfd == oldfd, just return newfd
    if (oldfd == newfd) {
        spinlock_release_irqrestore(&current->fd_table_lock, flags);
        return newfd;
    }
    
    // Close existing newfd if it's open
    if (current->fd_table[newfd]) {
        // TODO: Close the file descriptor properly
        current->fd_table[newfd] = NULL;
    }
    
    // Duplicate the file descriptor
    current->fd_table[newfd] = old_sf; // Simple duplication - same file object
    
    spinlock_release_irqrestore(&current->fd_table_lock, flags);
    
    serial_printf("[Dup2] Duplicated fd %u to fd %u for PID %u\n", oldfd, newfd, current->pid);
    return newfd;
}