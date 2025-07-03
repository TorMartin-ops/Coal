/**
 * @file syscall_filesystem.c
 * @brief File system related system call implementations
 * @author Coal OS Kernel Team
 * @version 1.0
 * 
 * @details Implements system calls for file and directory operations including
 * mkdir, rmdir, unlink, stat, chdir, getcwd, and readdir.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/cpu/isr_frame.h>
#include "syscall_utils.h"
#include "syscall_security.h"
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/dirent.h>
// #include <kernel/fs/stat.h> - removed due to conflict with sys/stat.h
#include <kernel/process/process.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/uaccess.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/string.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <libc/limits.h>

//============================================================================
// System Call Implementations
//============================================================================

/**
 * @brief Create a directory
 * @param user_pathname_ptr User-space pointer to pathname string
 * @param mode Directory permissions (currently ignored)
 * @param arg3 Unused
 * @param regs Interrupt frame
 * @return 0 on success, negative error code on failure
 */
int32_t sys_mkdir_impl(uint32_t user_pathname_ptr, uint32_t mode, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg3; (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Copy pathname from user space with enhanced security checks
    char pathname[SYSCALL_MAX_PATH_LEN];
    int copy_result = syscall_copy_path_from_user((const_userptr_t)user_pathname_ptr, 
                                                  pathname, sizeof(pathname));
    if (copy_result < 0) {
        serial_printf("[sys_mkdir] Failed to copy pathname: error %d\n", copy_result);
        return copy_result;
    }
    
    serial_printf("[sys_mkdir] Creating directory: '%s' with mode 0%o\n", pathname, mode);
    
    // Use VFS layer mkdir function
    int result = vfs_mkdir(pathname, mode);
    
    // Convert VFS error codes to Linux errno values
    switch (result) {
        case 0:
            return 0; // Success
        case -FS_ERR_NOT_FOUND:
            return -ENOENT;
        case -FS_ERR_FILE_EXISTS:
            return -EEXIST;
        case -FS_ERR_NOT_A_DIRECTORY:
            return -ENOTDIR;
        case -FS_ERR_PERMISSION_DENIED:
            return -EACCES;
        case -FS_ERR_IO:
            return -EIO;
        case -FS_ERR_NO_SPACE:
            return -ENOSPC;
        case -FS_ERR_NOT_SUPPORTED:
            return -ENOSYS;
        default:
            return -EIO; // Generic I/O error for unmapped errors
    }
}

/**
 * @brief Remove a directory
 * @param user_pathname_ptr User-space pointer to pathname string
 * @param arg2 Unused
 * @param arg3 Unused
 * @param regs Interrupt frame
 * @return 0 on success, negative error code on failure
 */
int32_t sys_rmdir_impl(uint32_t user_pathname_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg2; (void)arg3; (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Copy pathname from user space with enhanced security checks
    char pathname[SYSCALL_MAX_PATH_LEN];
    int copy_result = syscall_copy_path_from_user((const_userptr_t)user_pathname_ptr, 
                                                  pathname, sizeof(pathname));
    if (copy_result < 0) {
        serial_printf("[sys_rmdir] Failed to copy pathname: error %d\n", copy_result);
        return copy_result;
    }
    
    serial_printf("[sys_rmdir] Removing directory: '%s'\n", pathname);
    
    // Use VFS layer rmdir function
    int result = vfs_rmdir(pathname);
    
    // Convert VFS error codes to Linux errno values
    switch (result) {
        case 0:
            return 0; // Success
        case -FS_ERR_NOT_FOUND:
            return -ENOENT;
        case -FS_ERR_NOT_A_DIRECTORY:
            return -ENOTDIR;
        case -FS_ERR_BUSY:  // Directory not empty is often reported as BUSY
            return -ENOTEMPTY;
        case -FS_ERR_PERMISSION_DENIED:
            return -EACCES;
        case -FS_ERR_IO:
            return -EIO;
        case -FS_ERR_NOT_SUPPORTED:
            return -ENOSYS;
        default:
            return -EIO; // Generic I/O error for unmapped errors
    }
}

/**
 * @brief Remove a file
 * @param user_pathname_ptr User-space pointer to pathname string
 * @param arg2 Unused
 * @param arg3 Unused
 * @param regs Interrupt frame
 * @return 0 on success, negative error code on failure
 */
int32_t sys_unlink_impl(uint32_t user_pathname_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg2; (void)arg3; (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Copy pathname from user space with enhanced security checks
    char pathname[SYSCALL_MAX_PATH_LEN];
    int copy_result = syscall_copy_path_from_user((const_userptr_t)user_pathname_ptr, 
                                                  pathname, sizeof(pathname));
    if (copy_result < 0) {
        serial_printf("[sys_unlink] Failed to copy pathname: error %d\n", copy_result);
        return copy_result;
    }
    
    serial_printf("[sys_unlink] Unlinking file: '%s'\n", pathname);
    
    // Use VFS layer unlink function
    int result = vfs_unlink(pathname);
    
    // Convert VFS error codes to Linux errno values
    switch (result) {
        case 0:
            return 0; // Success
        case -FS_ERR_NOT_FOUND:
            return -ENOENT;
        case -FS_ERR_IS_A_DIRECTORY:
            return -EISDIR;
        case -FS_ERR_PERMISSION_DENIED:
            return -EACCES;
        case -FS_ERR_IO:
            return -EIO;
        case -FS_ERR_NOT_SUPPORTED:
            return -ENOSYS;
        default:
            return -EIO; // Generic I/O error for unmapped errors
    }
}

/**
 * @brief Get file status
 * @param user_path_ptr User-space pointer to pathname string
 * @param user_stat_ptr User-space pointer to stat structure
 * @param arg3 Unused
 * @param regs Interrupt frame
 * @return 0 on success, negative error code on failure
 */
int32_t sys_stat_impl(uint32_t user_path_ptr, uint32_t user_stat_ptr, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg3; (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Validate stat buffer pointer
    if (!syscall_validate_buffer((userptr_t)user_stat_ptr, sizeof(struct stat), true)) {
        serial_printf("[sys_stat] Invalid stat buffer pointer: 0x%x\n", user_stat_ptr);
        return -EFAULT;
    }
    
    // Copy pathname from user space with enhanced security checks
    char pathname[SYSCALL_MAX_PATH_LEN];
    int copy_result = syscall_copy_path_from_user((const_userptr_t)user_path_ptr, 
                                                  pathname, sizeof(pathname));
    if (copy_result < 0) {
        serial_printf("[sys_stat] Failed to copy pathname: error %d\n", copy_result);
        return copy_result;
    }
    
    serial_printf("[sys_stat] Getting file status for: '%s'\n", pathname);
    
    // TODO: Implement stat functionality
    serial_printf("[sys_stat] stat not yet implemented\n");
    return -ENOSYS;
}

/**
 * @brief Change current working directory
 * @param user_path_ptr User-space pointer to pathname string
 * @param arg2 Unused
 * @param arg3 Unused
 * @param regs Interrupt frame
 * @return 0 on success, negative error code on failure
 */
int32_t sys_chdir_impl(uint32_t user_path_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg2; (void)arg3; (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Copy pathname from user space with enhanced security checks
    char pathname[SYSCALL_MAX_PATH_LEN];
    int copy_result = syscall_copy_path_from_user((const_userptr_t)user_path_ptr, 
                                                  pathname, sizeof(pathname));
    if (copy_result < 0) {
        serial_printf("[sys_chdir] Failed to copy pathname: error %d\n", copy_result);
        return copy_result;
    }
    
    serial_printf("[sys_chdir] Changing directory to: '%s'\n", pathname);
    
    // TODO: Implement current working directory tracking in PCB
    serial_printf("[sys_chdir] chdir not yet implemented\n");
    return -ENOSYS;
}

/**
 * @brief Get current working directory
 * @param user_buf_ptr User-space pointer to buffer
 * @param size Size of buffer
 * @param arg3 Unused
 * @param regs Interrupt frame
 * @return Buffer pointer on success, NULL on failure
 */
int32_t sys_getcwd_impl(uint32_t user_buf_ptr, uint32_t size, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg3; (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Validate user buffer with enhanced security checks
    if (!syscall_validate_buffer((userptr_t)user_buf_ptr, size, true)) {
        serial_printf("[sys_getcwd] Invalid buffer: ptr=0x%x, size=%u\n", user_buf_ptr, size);
        return -EFAULT;
    }
    
    // Additional check for reasonable buffer size
    if (size > SYSCALL_MAX_PATH_LEN) {
        serial_printf("[sys_getcwd] Buffer size too large: %u\n", size);
        return -EINVAL;
    }
    
    serial_printf("[sys_getcwd] Getting current working directory\n");
    
    // TODO: Implement current working directory tracking
    // For now, return root directory
    const char* cwd = "/";
    size_t cwd_len = strlen(cwd) + 1;
    
    if (size < cwd_len) {
        return -ERANGE;
    }
    
    // Copy to user space
    if (copy_to_user((userptr_t)user_buf_ptr, (const kernelptr_t)cwd, cwd_len) != 0) {
        return -EFAULT;
    }
    
    return (int32_t)user_buf_ptr;
}

/**
 * @brief Read directory entries
 * @param fd File descriptor of directory
 * @param user_dirp User-space pointer to linux_dirent buffer
 * @param count Size of buffer
 * @param regs Interrupt frame
 * @return Number of bytes read on success, negative error code on failure
 */
int32_t sys_getdents_impl(uint32_t fd, uint32_t user_dirp, uint32_t count, isr_frame_t *regs)
{
    (void)regs;
    
    // Get current process
    pcb_t* current_proc = get_current_process();
    if (!current_proc) {
        return -ESRCH;
    }
    
    // Validate file descriptor
    if (fd >= MAX_FD || !current_proc->fd_table[fd]) {
        serial_printf("[sys_getdents] Invalid file descriptor: %u\n", fd);
        return -EBADF;
    }
    
    // Validate user buffer with enhanced security checks
    if (!syscall_validate_buffer((userptr_t)user_dirp, count, true)) {
        serial_printf("[sys_getdents] Invalid buffer: ptr=0x%x, count=%u\n", user_dirp, count);
        return -EFAULT;
    }
    
    // Additional check for reasonable buffer size
    if (count > SYSCALL_MAX_BUFFER_LEN) {
        serial_printf("[sys_getdents] Buffer size too large: %u\n", count);
        return -EINVAL;
    }
    
    serial_printf("[sys_getdents] Reading directory entries from fd %u\n", fd);
    
    // TODO: Implement getdents functionality
    // This requires proper integration with VFS readdir
    serial_printf("[sys_getdents] getdents not yet implemented\n");
    return -ENOSYS;
}