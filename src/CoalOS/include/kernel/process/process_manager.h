/**
 * @file process_manager.h
 * @brief Header file for the decomposed process management modules
 * 
 * Declares functions for the separated process management components.
 * Each module focuses on a specific responsibility following SRP.
 */

#ifndef KERNEL_PROCESS_MANAGER_H
#define KERNEL_PROCESS_MANAGER_H

#include <kernel/process/process.h>
#include <kernel/core/error.h>

// ============================================================================
// PCB Management (process_pcb_manager.c)
// ============================================================================

/**
 * @brief Allocates and initializes a basic PCB structure
 * @param name Process name (for debugging)
 * @return Pointer to the newly allocated PCB, or NULL on failure
 */
pcb_t* process_create(const char* name);

/**
 * @brief Gets the PCB of the currently running process
 * @return Pointer to the current PCB, or NULL if no process context is active
 */
pcb_t* get_current_process(void);

/**
 * @brief Initializes process hierarchy fields in a new PCB
 * @param proc Process control block to initialize
 */
void process_init_hierarchy(pcb_t *proc);

// === New Standardized Process Management API ===

/**
 * @brief Allocates and initializes a PCB with standardized error reporting
 * @param name Process name (for debugging), must not be NULL
 * @param proc_out Output parameter for the newly allocated PCB
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: Invalid input parameters (name is NULL or proc_out is NULL)
 *         - E_NOMEM: Insufficient memory for PCB allocation
 *         - E_OVERFLOW: PID counter overflow (system limit reached)
 */
error_t process_create_safe(const char* name, pcb_t** proc_out);

/**
 * @brief Gets the currently running process with error reporting
 * @param proc_out Output parameter for the current process PCB
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: proc_out is NULL
 *         - E_NOTFOUND: No current process context active
 *         - E_FAULT: Scheduler state corruption detected
 */
error_t get_current_process_safe(pcb_t** proc_out);

// ============================================================================
// Memory Management (process_memory.c)
// ============================================================================

/**
 * @brief Allocates and maps kernel stack for a process
 * @param proc Pointer to the PCB to setup the kernel stack for
 * @return true on success, false on failure
 */
bool allocate_kernel_stack(pcb_t *proc);

/**
 * @brief Frees kernel stack resources for a process
 * @param proc Process whose kernel stack should be freed
 */
void free_kernel_stack(pcb_t *proc);

/**
 * @brief Allocates and maps kernel stack with standardized error reporting
 * @param proc Pointer to the PCB to setup the kernel stack for
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: proc is NULL or already has a kernel stack
 *         - E_NOMEM: Insufficient memory for stack allocation
 *         - E_FAULT: Memory mapping failure
 */
error_t allocate_kernel_stack_safe(pcb_t *proc);

// ============================================================================
// File Descriptor Management (process_fd_manager.c)  
// ============================================================================

/**
 * @brief Initializes the file descriptor table for a new process
 * @param proc Pointer to the new process's PCB
 */
void process_init_fds(pcb_t *proc);

/**
 * @brief Closes all open file descriptors for a terminating process
 * @param proc Pointer to the terminating process's PCB
 */
void process_close_fds(pcb_t *proc);

// ============================================================================
// Process Hierarchy Management (process_hierarchy.c)
// ============================================================================

/**
 * @brief Establishes parent-child relationship between processes
 * @param parent Parent process
 * @param child Child process
 */
void process_add_child(pcb_t *parent, pcb_t *child);

/**
 * @brief Removes a child from parent's children list
 * @param parent Parent process 
 * @param child Child process to remove
 */
void process_remove_child(pcb_t *parent, pcb_t *child);

/**
 * @brief Finds a child process by PID
 * @param parent Parent process
 * @param child_pid PID of child to find
 * @return Pointer to child PCB, or NULL if not found
 */
pcb_t *process_find_child(pcb_t *parent, uint32_t child_pid);

/**
 * @brief Marks a process as exited and notifies parent
 * @param proc Process that is exiting
 * @param exit_status Exit status code
 */
void process_exit_with_status(pcb_t *proc, uint32_t exit_status);

/**
 * @brief Reaps zombie children and cleans up their resources
 * @param parent Parent process
 * @param child_pid PID of child to reap (-1 for any child)
 * @param status Pointer to store exit status
 * @return PID of reaped child, or negative error code
 */
int process_reap_child(pcb_t *parent, int child_pid, int *status);

// ============================================================================
// Process Groups and Sessions (process_groups.c)
// ============================================================================

/**
 * @brief Initializes process group and session fields in a new PCB
 * @param proc Process to initialize
 * @param parent Parent process (NULL for init process)
 */
void process_init_pgrp_session(pcb_t *proc, pcb_t *parent);

/**
 * @brief Creates a new session with the calling process as leader
 * @param proc Process to become session leader
 * @return Session ID on success, negative error code on failure
 */
int process_setsid(pcb_t *proc);

/**
 * @brief Gets the session ID of a process
 * @param proc Process to query
 * @return Session ID
 */
uint32_t process_getsid(pcb_t *proc);

/**
 * @brief Sets the process group ID of a process
 * @param proc Process to modify
 * @param pgid New process group ID
 * @return 0 on success, negative error code on failure
 */
int process_setpgid(pcb_t *proc, uint32_t pgid);

/**
 * @brief Gets the process group ID of a process
 * @param proc Process to query
 * @return Process group ID
 */
uint32_t process_getpgid(pcb_t *proc);

/**
 * @brief Joins a process to a process group
 * @param proc Process to add to group
 * @param pgrp_leader Process group leader
 * @return 0 on success, negative error code on failure
 */
int process_join_pgrp(pcb_t *proc, pcb_t *pgrp_leader);

/**
 * @brief Removes a process from its current process group
 * @param proc Process to remove from group
 */
void process_leave_pgrp(pcb_t *proc);

/**
 * @brief Sets the foreground process group for a terminal
 * @param proc Session leader process
 * @param pgid Process group ID to set as foreground
 * @return 0 on success, negative error code on failure
 */
int process_tcsetpgrp(pcb_t *proc, uint32_t pgid);

/**
 * @brief Gets the foreground process group for a terminal
 * @param proc Process with controlling terminal
 * @return Process group ID, or negative error code
 */
int process_tcgetpgrp(pcb_t *proc);

// ============================================================================
// High-Level Process Creation (process_creation.c)
// ============================================================================

/**
 * @brief Creates a new user process by loading an ELF executable
 * @param path Path to the executable file
 * @return Pointer to the newly created PCB on success, NULL on failure
 */
pcb_t *create_user_process(const char *path);

/**
 * @brief Destroys a process and frees all associated resources
 * @param pcb Pointer to the PCB of the process to destroy
 */
void destroy_process(pcb_t *pcb);

/**
 * @brief Creates a new user process with standardized error reporting
 * @param path Path to the executable file
 * @param proc_out Output parameter for the newly created PCB
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: path is NULL or proc_out is NULL
 *         - E_NOTFOUND: Executable file not found
 *         - E_NOMEM: Insufficient memory for process creation
 *         - E_FAULT: ELF loading or memory mapping failure
 *         - E_CORRUPT: Invalid or corrupted ELF file
 */
error_t create_user_process_safe(const char *path, pcb_t **proc_out);

#endif // KERNEL_PROCESS_MANAGER_H