/**
 * @file process.c
 * @brief Process Management - Modular Implementation
 *
 * This file now serves as a compatibility layer for the refactored process management
 * system. The original monolithic process.c has been decomposed into focused modules
 * following the Single Responsibility Principle:
 *
 * - process_pcb_manager.c: PCB allocation and basic management
 * - process_memory.c: Kernel stack allocation and memory management  
 * - process_fd_manager.c: File descriptor table management
 * - process_hierarchy.c: Parent-child relationships and process reaping
 * - process_groups.c: Process groups and sessions for POSIX compatibility
 * - process_creation.c: High-level process creation orchestration
 *
 * This refactoring improves maintainability, testability, and follows SOLID principles.
 *
 * NOTE: The original 1364-line monolithic file has been moved to process_original.c
 * for reference. All functionality is now provided by the modular components.
 */

#include <kernel/process/process_manager.h>

// All process management functions are now implemented in their respective modules:
//
// Functions from process_pcb_manager.c:
// - process_create()
// - get_current_process()
// - process_init_hierarchy()
//
// Functions from process_memory.c:
// - allocate_kernel_stack()
// - free_kernel_stack()
//
// Functions from process_fd_manager.c:
// - process_init_fds()
// - process_close_fds()
//
// Functions from process_hierarchy.c:
// - process_add_child()
// - process_remove_child()
// - process_find_child()
// - process_exit_with_status()
// - process_reap_child()
//
// Functions from process_groups.c:
// - process_init_pgrp_session()
// - process_setsid()
// - process_getsid()
// - process_setpgid()
// - process_getpgid()
// - process_join_pgrp()
// - process_leave_pgrp()
// - process_tcsetpgrp()
// - process_tcgetpgrp()
//
// Functions from process_creation.c:
// - create_user_process()
// - destroy_process()

// This modular approach provides several benefits:
// 1. Single Responsibility: Each module focuses on one specific aspect
// 2. Maintainability: Easier to find, understand, and modify specific functionality
// 3. Testability: Each module can be tested independently
// 4. Reusability: Modules can be reused in different contexts
// 5. Scalability: New functionality can be added without modifying existing modules