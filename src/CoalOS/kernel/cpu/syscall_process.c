/**
 * @file syscall_process.c
 * @brief Process Management System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles process-related system calls including fork, execve, exit,
 * waitpid, and process identification functions.
 */

//============================================================================
// Includes
//============================================================================
#include "syscall_process.h"
#include "syscall_utils.h"
#include "syscall_security.h"
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/process/elf_loader.h>
#include <kernel/memory/uaccess.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/paging_process.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>

//============================================================================
// Process Lifecycle Implementation
//============================================================================

int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        serial_printf("[Exit] sys_exit called without process context! Ignoring.\n");
        return -EFAULT;
    }
    
    serial_printf("[Exit] Process PID %u exiting with code %u\n", current->pid, code);
    
    // Mark the current task as zombie and remove it from scheduler
    // This function will not return as it performs a context switch
    remove_current_task_with_code(code);
    
    // Should never reach here
    return 0;
}

int32_t sys_fork_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3;
    
    pcb_t *parent = get_current_process();
    if (!parent) {
        return -ESRCH;
    }
    
    serial_printf("[Fork] Creating child process for PID %u\n", parent->pid);
    
    // Create child process - use a simple stub for now
    pcb_t *child = NULL; // TODO: Implement proper process creation
    if (!child) {
        serial_printf("[Fork] Process creation not implemented\n");
        return -ENOSYS;
    }
    
    // Copy memory management structures
    int mm_result = copy_mm(parent, child);
    if (mm_result < 0) {
        serial_printf("[Fork] Failed to copy memory management structures: %d\n", mm_result);
        destroy_process(child);
        return mm_result;
    }
    
    // Copy file descriptor table
    int fd_result = copy_fd_table(parent, child);
    if (fd_result < 0) {
        serial_printf("[Fork] Failed to copy file descriptor table: %d\n", fd_result);
        destroy_process(child);
        return fd_result;
    }
    
    // TODO: Set up child's register state to return 0
    // child->registers = *regs;
    // child->registers.eax = 0; // Child returns 0
    
    // Set parent-child relationship
    child->ppid = parent->pid;
    
    // TODO: Add child to scheduler
    // scheduler_add_task(child);
    
    serial_printf("[Fork] Successfully created child PID %u from parent PID %u\n", 
                  child->pid, parent->pid);
    
    // Parent returns child PID
    return (int32_t)child->pid;
}

int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs)
{
    (void)user_envp_ptr; (void)regs;
    
    const_userptr_t user_pathname = (const_userptr_t)user_pathname_ptr;
    char k_pathname[SYSCALL_MAX_PATH_LEN];
    char **argv = NULL;
    int argc = 0;
    
    // Copy pathname from user space with enhanced security checks
    int copy_err = syscall_copy_path_from_user(user_pathname, k_pathname, sizeof(k_pathname));
    if (copy_err != 0) {
        serial_printf("[sys_execve] Failed to copy pathname: error %d\n", copy_err);
        return copy_err;
    }
    
    // Validate and parse argv if provided
    if (user_argv_ptr != 0) {
        // First validate the argv array
        int argv_count = syscall_validate_string_array((const_userptr_t)user_argv_ptr, 
                                                       SYSCALL_MAX_ARGV_COUNT, 
                                                       SYSCALL_MAX_ARG_LEN);
        if (argv_count < 0) {
            serial_printf("[sys_execve] Invalid argv array: error %d\n", argv_count);
            return argv_count;
        }
        
        int argv_err = parse_argv(user_argv_ptr, &argv, &argc);
        if (argv_err != 0) {
            return argv_err;
        }
    }
    
    pcb_t *current = get_current_process();
    if (!current) {
        if (argv) free_argv(argv, argc);
        return -ESRCH;
    }
    
    serial_printf("[Execve] Loading %s for PID %u\n", k_pathname, current->pid);
    
    // TODO: Load new program - this will replace current process image
    // int load_result = elf_load_program(current, k_pathname);
    // if (load_result < 0) {
    //     serial_printf("[Execve] Failed to load program %s: %d\n", k_pathname, load_result);
    //     if (argv) free_argv(argv, argc);
    //     return load_result;
    // }
    
    serial_printf("[Execve] ELF loading not implemented for %s\n", k_pathname);
    
    // Clean up argv
    if (argv) free_argv(argv, argc);
    
    // execve is not implemented yet
    return -ENOSYS;
}

int32_t sys_waitpid_impl(uint32_t pid, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs)
{
    (void)regs;
    
    userptr_t user_status = (userptr_t)user_status_ptr;
    
    // Enhanced validation
    if (user_status && !syscall_validate_buffer(user_status, sizeof(int), true)) {
        serial_printf("[sys_waitpid] Invalid status buffer: ptr=0x%x\n", user_status_ptr);
        return -EFAULT;
    }
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    // Check for obviously invalid PIDs (garbage values)
    if (pid > 10000) {
        serial_printf("[Waitpid] PID %u attempted to wait for invalid child PID %u (too large)\n", 
                      current->pid, pid);
        return -ECHILD;
    }
    
    // TODO: Implement proper waitpid logic with process hierarchy
    // For now, if the caller is repeatedly calling waitpid for non-existent children,
    // we should exit the process to prevent infinite loops
    static uint32_t waitpid_call_count = 0;
    waitpid_call_count++;
    
    serial_printf("[Waitpid] PID %u waiting for child PID %u (not implemented, call #%u)\n", 
                  current->pid, pid, waitpid_call_count);
    
    // If a process makes too many waitpid calls for non-existent children, exit it
    if (waitpid_call_count > 10) {
        serial_printf("[Waitpid] PID %u made too many waitpid calls for non-existent children, exiting process\n", 
                      current->pid);
        return sys_exit_impl(1, 0, 0, regs); // Exit with error code 1
    }
    
    (void)options; // Suppress unused parameter warning
    
    return -ECHILD; // No child processes
}

//============================================================================
// Process Identification Implementation
//============================================================================

int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    return (int32_t)current->pid;
}

int32_t sys_getppid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs)
{
    (void)arg1; (void)arg2; (void)arg3; (void)regs;
    
    pcb_t *current = get_current_process();
    if (!current) {
        return -ESRCH;
    }
    
    return (int32_t)current->ppid;
}