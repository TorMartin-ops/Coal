/**
 * @file scheduler_cleanup.h
 * @brief Zombie Process Cleanup Interface
 * @author Refactored for SOLID principles
 * @version 6.0
 */

#ifndef SCHEDULER_CLEANUP_H
#define SCHEDULER_CLEANUP_H

#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Cleanup Functions
//============================================================================

/**
 * @brief Clean up one zombie task if available
 * @note Called periodically by idle task or scheduler
 */
void scheduler_cleanup_zombies(void);

/**
 * @brief Clean up all available zombie tasks
 * @note More aggressive cleanup for system shutdown or maintenance
 */
void scheduler_cleanup_all_zombies(void);

/**
 * @brief Force cleanup of a specific task by PID
 * @param pid Process ID to forcibly clean up
 * @return True if task was found and cleaned up
 */
bool scheduler_cleanup_force_cleanup_task(uint32_t pid);

//============================================================================
// Cleanup Statistics & Monitoring
//============================================================================

/**
 * @brief Get cleanup statistics
 * @param total_reaped Total number of tasks reaped (can be NULL)
 * @param last_reap_tick Tick of last successful reap (can be NULL)
 * @param failures Number of cleanup failures (can be NULL)
 */
void scheduler_cleanup_get_stats(uint32_t *total_reaped, uint32_t *last_reap_tick, uint32_t *failures);

/**
 * @brief Increment cleanup statistics
 * @param success True if cleanup was successful
 */
void scheduler_cleanup_increment_stats(bool success);

/**
 * @brief Print cleanup statistics for debugging
 */
void scheduler_cleanup_print_stats(void);

//============================================================================
// Resource Validation
//============================================================================

/**
 * @brief Validate task resources before cleanup
 * @param task Task to validate
 * @return True if task is safe to clean up
 */
bool scheduler_cleanup_validate_task_resources(tcb_t *task);

#endif // SCHEDULER_CLEANUP_H