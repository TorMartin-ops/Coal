/**
 * @file syscall_process.h
 * @brief Process Management System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Headers for process-related system calls including fork, execve, exit,
 * waitpid, and process identification functions.
 */

#ifndef SYSCALL_PROCESS_H
#define SYSCALL_PROCESS_H

//============================================================================
// Includes
//============================================================================
#include <kernel/cpu/isr_frame.h>
#include <libc/stdint.h>

//============================================================================
// Process Lifecycle Functions
//============================================================================

/**
 * @brief Exit current process with specified exit code
 * @param code Exit status code
 * @param arg2 Unused
 * @param arg3 Unused
 * @param regs Interrupt register frame
 * @return Does not return on success
 */
int32_t sys_exit_impl(uint32_t code, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

/**
 * @brief Create a new process by duplicating current process
 * @param arg1 Unused
 * @param arg2 Unused  
 * @param arg3 Unused
 * @param regs Interrupt register frame
 * @return Child PID to parent, 0 to child, negative error code on failure
 */
int32_t sys_fork_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

/**
 * @brief Replace current process image with new program
 * @param user_pathname_ptr User space pointer to program path
 * @param user_argv_ptr User space pointer to argument array
 * @param user_envp_ptr User space pointer to environment array
 * @param regs Interrupt register frame
 * @return Does not return on success, negative error code on failure
 */
int32_t sys_execve_impl(uint32_t user_pathname_ptr, uint32_t user_argv_ptr, uint32_t user_envp_ptr, isr_frame_t *regs);

/**
 * @brief Wait for child process to terminate
 * @param pid Process ID to wait for
 * @param user_status_ptr User space pointer to store exit status
 * @param options Wait options
 * @param regs Interrupt register frame
 * @return PID of terminated child, or negative error code
 */
int32_t sys_waitpid_impl(uint32_t pid, uint32_t user_status_ptr, uint32_t options, isr_frame_t *regs);

//============================================================================
// Process Identification Functions
//============================================================================

/**
 * @brief Get current process ID
 * @param arg1 Unused
 * @param arg2 Unused
 * @param arg3 Unused
 * @param regs Interrupt register frame
 * @return Current process PID
 */
int32_t sys_getpid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

/**
 * @brief Get parent process ID
 * @param arg1 Unused
 * @param arg2 Unused
 * @param arg3 Unused
 * @param regs Interrupt register frame
 * @return Parent process PID
 */
int32_t sys_getppid_impl(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

#endif // SYSCALL_PROCESS_H