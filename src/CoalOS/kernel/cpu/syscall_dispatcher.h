/**
 * @file syscall_dispatcher.h
 * @brief System Call Dispatcher and Registration
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Core system call dispatcher that manages the syscall table,
 * handles Linux compatibility mode, and routes system calls to appropriate
 * handler modules. Follows Facade pattern for coordinating syscall modules.
 */

#ifndef SYSCALL_DISPATCHER_H
#define SYSCALL_DISPATCHER_H

//============================================================================
// Includes
//============================================================================
#include <kernel/cpu/isr_frame.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// System Call Configuration
//============================================================================
#define MAX_SYSCALLS 512

// System call function pointer type
typedef int32_t (*syscall_fn_t)(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

//============================================================================
// Dispatcher Functions
//============================================================================

/**
 * @brief Initialize the system call dispatcher and register all handlers
 */
void syscall_init(void);

/**
 * @brief Main system call dispatcher entry point
 * @param regs Interrupt register frame containing syscall number and arguments
 */
int32_t syscall_dispatcher(isr_frame_t *regs);

/**
 * @brief Enable or disable Linux compatibility mode
 * @param enable True to enable Linux compatibility, false to disable
 */
void syscall_set_linux_compat(bool enable);

/**
 * @brief Check if Linux compatibility mode is enabled
 * @return True if Linux compatibility is enabled, false otherwise
 */
bool syscall_get_linux_compat(void);

//============================================================================
// Registration Functions
//============================================================================

/**
 * @brief Register a system call handler
 * @param syscall_num System call number
 * @param handler Function pointer to the handler
 * @return 0 on success, negative error code on failure
 */
int syscall_register(uint32_t syscall_num, syscall_fn_t handler);

/**
 * @brief Unregister a system call handler
 * @param syscall_num System call number to unregister
 */
void syscall_unregister(uint32_t syscall_num);

/**
 * @brief Get the handler for a specific system call
 * @param syscall_num System call number
 * @return Function pointer to handler, or NULL if not registered
 */
syscall_fn_t syscall_get_handler(uint32_t syscall_num);

//============================================================================
// Default Handlers
//============================================================================

/**
 * @brief Default handler for unimplemented system calls
 * @param arg1 First argument
 * @param arg2 Second argument
 * @param arg3 Third argument
 * @param regs Interrupt register frame
 * @return -ENOSYS (function not implemented)
 */
int32_t sys_not_implemented(uint32_t arg1, uint32_t arg2, uint32_t arg3, isr_frame_t *regs);

#endif // SYSCALL_DISPATCHER_H