/**
 * @file syscall_stubs.c
 * @brief Stub implementations for unimplemented system calls
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Provides stub implementations that return -ENOSYS for system calls
 * that have not yet been fully implemented in their respective modules.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/cpu/isr_frame.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/core/types.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>

// Forward declarations for types
typedef struct vnode vnode_t;

//============================================================================
// Terminal Operations Stubs
//============================================================================

int32_t sys_puts_impl(uint32_t user_str_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)user_str_ptr; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_puts not implemented\n");
    return -ENOSYS;
}

int32_t sys_read_terminal_line_impl(uint32_t user_buf_ptr, uint32_t count, uint32_t arg3, isr_frame_t *regs)
{
    (void)user_buf_ptr; (void)count; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_read_terminal_line not implemented\n");
    return -ENOSYS;
}

//============================================================================
// Pipe Operations Stubs
//============================================================================

int32_t sys_pipe_impl(uint32_t user_pipefd_ptr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)user_pipefd_ptr; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_pipe not implemented\n");
    return -ENOSYS;
}

ssize_t pipe_read_operation(void *vnode, void *buffer, size_t count, off_t offset)
{
    (void)vnode; (void)buffer; (void)count; (void)offset;
    serial_write("[Syscall] pipe_read_operation not implemented\n");
    return -ENOSYS;
}

ssize_t pipe_write_operation(void *vnode, const void *buffer, size_t count, off_t offset)
{
    (void)vnode; (void)buffer; (void)count; (void)offset;
    serial_write("[Syscall] pipe_write_operation not implemented\n");
    return -ENOSYS;
}

int pipe_close_operation(void *vnode, bool is_write_end)
{
    (void)vnode; (void)is_write_end;
    serial_write("[Syscall] pipe_close_operation not implemented\n");
    return -ENOSYS;
}

//============================================================================
// Signal Handling Stubs
//============================================================================

int32_t sys_signal_impl(uint32_t signum, uint32_t user_handler_ptr, uint32_t arg3, isr_frame_t *regs)
{
    (void)signum; (void)user_handler_ptr; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_signal not implemented\n");
    return -ENOSYS;
}

int32_t sys_kill_impl(uint32_t pid, uint32_t sig, uint32_t arg3, isr_frame_t *regs)
{
    (void)pid; (void)sig; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_kill not implemented\n");
    return -ENOSYS;
}

//============================================================================
// File System Operations Stubs
//============================================================================
// NOTE: sys_chdir, sys_getcwd, sys_stat, sys_mkdir, sys_rmdir, sys_unlink
// are now implemented in syscall_filesystem.c

int32_t sys_readdir_impl(uint32_t fd, uint32_t user_buf_ptr, uint32_t count, isr_frame_t *regs)
{
    (void)fd; (void)user_buf_ptr; (void)count; (void)regs;
    serial_write("[Syscall] sys_readdir not implemented\n");
    return -ENOSYS;
}

//============================================================================
// Process Groups and Sessions Stubs
//============================================================================

int32_t sys_setsid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_setsid not implemented\n");
    return -ENOSYS;
}

int32_t sys_getsid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)pid; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_getsid not implemented\n");
    return -ENOSYS;
}

int32_t sys_setpgid_impl(uint32_t pid, uint32_t pgid, uint32_t arg3, isr_frame_t *regs)
{
    (void)pid; (void)pgid; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_setpgid not implemented\n");
    return -ENOSYS;
}

int32_t sys_getpgid_impl(uint32_t pid, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)pid; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_getpgid not implemented\n");
    return -ENOSYS;
}

int32_t sys_getpgrp_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_getpgrp not implemented\n");
    return -ENOSYS;
}

int32_t sys_tcsetpgrp_impl(uint32_t fd, uint32_t pgid, uint32_t arg3, isr_frame_t *regs)
{
    (void)fd; (void)pgid; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_tcsetpgrp not implemented\n");
    return -ENOSYS;
}

int32_t sys_tcgetpgrp_impl(uint32_t fd, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)fd; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_tcgetpgrp not implemented\n");
    return -ENOSYS;
}

//============================================================================
// Memory Management Stubs
//============================================================================

int32_t sys_brk_impl(uint32_t brk_addr, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)brk_addr; (void)arg2; (void)arg3; (void)regs;
    serial_write("[Syscall] sys_brk not implemented\n");
    return -ENOSYS;
}

int32_t sys_mmap_impl(uint32_t addr, uint32_t length, uint32_t prot, isr_frame_t *regs)
{
    (void)addr; (void)length; (void)prot; (void)regs;
    serial_write("[Syscall] sys_mmap not implemented\n");
    return -ENOSYS;
}