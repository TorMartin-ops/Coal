/**
 * @file syscall_fileio.h
 * @brief File I/O System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles file input/output system calls including read, write, open,
 * close, lseek, and file descriptor duplication. Focuses purely on file I/O
 * operations without terminal or pipe handling.
 */

#ifndef SYSCALL_FILEIO_H
#define SYSCALL_FILEIO_H

//============================================================================
// Includes
//============================================================================
#include <kernel/cpu/isr_frame.h>
#include <libc/stdint.h>

//============================================================================
// File I/O System Call Functions
//============================================================================

/**
 * @brief Read data from a file descriptor
 * @param fd File descriptor to read from
 * @param user_buf_ptr User space buffer pointer
 * @param count Number of bytes to read
 * @param regs Interrupt register frame
 * @return Number of bytes read on success, negative error code on failure
 */
int32_t sys_read_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);

/**
 * @brief Write data to a file descriptor
 * @param fd File descriptor to write to
 * @param user_buf_ptr User space buffer pointer
 * @param count Number of bytes to write
 * @param regs Interrupt register frame
 * @return Number of bytes written on success, negative error code on failure
 */
int32_t sys_write_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs);

/**
 * @brief Open a file or device
 * @param user_pathname_ptr User space path string pointer
 * @param flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, etc.)
 * @param mode File creation mode (permissions)
 * @param regs Interrupt register frame
 * @return File descriptor on success, negative error code on failure
 */
int32_t sys_open_impl(uint32_t user_pathname_ptr, uint32_t flags, uint32_t mode, isr_frame_t *regs);

/**
 * @brief Close a file descriptor
 * @param fd File descriptor to close
 * @param arg2 Unused argument
 * @param arg3 Unused argument
 * @param regs Interrupt register frame
 * @return 0 on success, negative error code on failure
 */
int32_t sys_close_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

/**
 * @brief Seek to a position in a file
 * @param fd File descriptor
 * @param offset Offset to seek to
 * @param whence Seek origin (SEEK_SET, SEEK_CUR, SEEK_END)
 * @param regs Interrupt register frame
 * @return New file position on success, negative error code on failure
 */
int32_t sys_lseek_impl(uint32_t fd, uint32_t offset, uint32_t whence, isr_frame_t *regs);

/**
 * @brief Duplicate a file descriptor
 * @param oldfd Old file descriptor
 * @param newfd New file descriptor number
 * @param arg3 Unused argument
 * @param regs Interrupt register frame
 * @return New file descriptor on success, negative error code on failure
 */
int32_t sys_dup2_impl(uint32_t oldfd, uint32_t newfd, uint32_t arg3, isr_frame_t *regs);

#endif // SYSCALL_FILEIO_H