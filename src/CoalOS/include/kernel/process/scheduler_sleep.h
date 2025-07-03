/**
 * @file scheduler_sleep.h
 * @brief Sleep Queue Management Interface
 * @author Refactored for SOLID principles
 * @version 6.0
 */

#ifndef SCHEDULER_SLEEP_H
#define SCHEDULER_SLEEP_H

#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Sleep Management Functions
//============================================================================

/**
 * @brief Initialize the sleep queue system
 */
void scheduler_sleep_init(void);

/**
 * @brief Check for tasks that need to be woken up
 * @note Called by scheduler tick handler
 */
void scheduler_sleep_check_wakeups(void);

/**
 * @brief Put current task to sleep for specified duration
 * @param ms Duration in milliseconds
 */
void scheduler_sleep_task(uint32_t ms);

//============================================================================
// Sleep Queue Statistics & Debug
//============================================================================

/**
 * @brief Get number of currently sleeping tasks
 * @return Number of tasks in sleep queue
 */
uint32_t scheduler_sleep_get_sleeping_count(void);

/**
 * @brief Print debug information about sleep queue
 */
void scheduler_sleep_debug_print_queue(void);

#endif // SCHEDULER_SLEEP_H