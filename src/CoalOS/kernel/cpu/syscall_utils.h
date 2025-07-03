/**
 * @file syscall_utils.h
 * @brief Shared Utilities for System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Common utility functions used across multiple system call modules
 * including user space access, memory management, and argument parsing.
 */

#ifndef SYSCALL_UTILS_H
#define SYSCALL_UTILS_H

//============================================================================
// Includes
//============================================================================
#include <kernel/cpu/isr_frame.h>
#include <kernel/memory/uaccess.h>
#include <kernel/process/process.h>
#include <libc/stdint.h>
#include <libc/stddef.h>

//============================================================================
// Constants
//============================================================================
#define MAX_SYSCALL_STR_LEN 256
#define MAX_ARGS 64

//============================================================================
// User Space Access Utilities
//============================================================================

/**
 * @brief Safely copy string from user space to kernel space
 * @param u_src User space source string pointer
 * @param k_dst Kernel space destination buffer
 * @param maxlen Maximum length to copy (including null terminator)
 * @return 0 on success, negative error code on failure
 */
int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen);

//============================================================================
// Process Memory Management
//============================================================================

/**
 * @brief Copy memory management structures from parent to child process
 * @param parent Parent process PCB
 * @param child Child process PCB
 * @return 0 on success, negative error code on failure
 */
int copy_mm(pcb_t *parent, pcb_t *child);

/**
 * @brief Simple VMA tree copy for process forking (internal helper)
 * This function is used internally by copy_mm and doesn't need to be exposed
 */

//============================================================================
// File Descriptor Management
//============================================================================

/**
 * @brief Copy file descriptor table from parent to child process
 * @param parent Parent process PCB
 * @param child Child process PCB
 * @return 0 on success, negative error code on failure
 */
int copy_fd_table(pcb_t *parent, pcb_t *child);

//============================================================================
// Argument Parsing
//============================================================================

/**
 * @brief Parse argv array from user space for execve
 * @param user_argv_ptr User space pointer to argv array
 * @param argv_out Output kernel array of argument strings
 * @param argc_out Output argument count
 * @return 0 on success, negative error code on failure
 */
int parse_argv(uint32_t user_argv_ptr, char ***argv_out, int *argc_out);

/**
 * @brief Free parsed argv array
 * @param argv Array of argument strings to free
 * @param argc Number of arguments
 */
void free_argv(char **argv, int argc);

//============================================================================
// Process Lookup
//============================================================================

/**
 * @brief Find process by PID (stub - should be moved to process.h)
 * @param pid Process ID to find
 * @return Pointer to PCB if found, NULL otherwise
 */
pcb_t *process_get_by_pid(uint32_t pid);

#endif // SYSCALL_UTILS_H