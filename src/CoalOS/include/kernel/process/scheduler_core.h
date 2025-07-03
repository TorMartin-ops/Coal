/**
 * @file scheduler_core.h
 * @brief Core Scheduling Logic Interface
 * @author Refactored for SOLID principles
 * @version 6.0
 */

#ifndef SCHEDULER_CORE_H
#define SCHEDULER_CORE_H

#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/core/error.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Core Scheduling Functions
//============================================================================

/**
 * @brief Selects the next task to run based on priority
 * @return Pointer to next task, or NULL if no tasks available
 */
tcb_t* scheduler_select_next_task(void);

/**
 * @brief Main scheduler function - performs task switching
 */
void schedule(void);

/**
 * @brief Scheduler tick handler - called by timer interrupt
 */
void scheduler_core_tick(void);

/**
 * @brief Adds a new task to the scheduler
 * @param pcb Process control block of the task to add
 * @return 0 on success, negative on error
 */
int scheduler_core_add_task(pcb_t *pcb);

/**
 * @brief Voluntarily yields CPU to another task
 */
void scheduler_core_yield(void);

/**
 * @brief Removes current task and marks it as zombie
 * @param code Exit code for the task
 */
void scheduler_core_remove_current_task(uint32_t code);

/**
 * @brief Unblocks a task and makes it ready to run
 * @param task Task to unblock
 */
void scheduler_core_unblock_task(tcb_t *task);

//============================================================================
// State Accessors
//============================================================================

/**
 * @brief Gets current task (volatile)
 * @return Volatile pointer to current task
 */
volatile tcb_t* scheduler_core_get_current_task_volatile(void);

/**
 * @brief Gets current task (non-volatile)
 * @return Pointer to current task
 */
tcb_t* scheduler_core_get_current_task(void);

/**
 * @brief Gets current tick count
 * @return Current system ticks
 */
uint32_t scheduler_core_get_ticks(void);

/**
 * @brief Sets the reschedule flag
 */
void scheduler_core_set_need_reschedule(void);

/**
 * @brief Checks if scheduler is ready
 * @return True if scheduler is ready
 */
bool scheduler_core_is_ready(void);

/**
 * @brief Sets scheduler ready state
 * @param ready Ready state to set
 */
void scheduler_core_set_ready(bool ready);

//============================================================================
// Priority Inheritance Functions
//============================================================================

/**
 * @brief Gets effective priority of a task
 * @param task Task to get priority for
 * @return Effective priority
 */
uint8_t scheduler_core_get_effective_priority(tcb_t *task);

/**
 * @brief Implements priority inheritance
 * @param holder Task holding resource
 * @param waiter Task waiting for resource
 */
void scheduler_core_inherit_priority(tcb_t *holder, tcb_t *waiter);

/**
 * @brief Restores original priority after resource release
 * @param task Task to restore priority for
 */
void scheduler_core_restore_priority(tcb_t *task);

/**
 * @brief Adds task to blocked tasks list
 * @param holder Resource holder
 * @param waiter Waiting task
 */
void scheduler_core_add_blocked_task(tcb_t *holder, tcb_t *waiter);

/**
 * @brief Removes task from blocked tasks list
 * @param holder Resource holder
 * @param waiter Previously waiting task
 */
void scheduler_core_remove_blocked_task(tcb_t *holder, tcb_t *waiter);

//============================================================================
// New Standardized Scheduler Error Handling API
//============================================================================

/**
 * @brief Adds a new task to the scheduler with standardized error reporting
 * @param pcb Process control block of the task to add
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: Invalid PCB or missing required fields
 *         - E_NOMEM: Insufficient memory for TCB allocation
 *         - E_FAULT: Failed to enqueue task in scheduler queues
 *         - E_EXIST: Task with same PID already exists in scheduler
 */
error_t scheduler_core_add_task_safe(pcb_t *pcb);

/**
 * @brief Selects the next task to run with error reporting
 * @param task_out Output parameter for the next task
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: task_out is NULL
 *         - E_NOTFOUND: No tasks available to run
 *         - E_FAULT: Scheduler state corruption detected
 */
error_t scheduler_select_next_task_safe(tcb_t **task_out);

/**
 * @brief Gets current task with validation and error reporting
 * @param task_out Output parameter for the current task
 * @return E_SUCCESS on success, specific error_t code on failure:
 *         - E_INVAL: task_out is NULL
 *         - E_NOTFOUND: No current task active
 *         - E_FAULT: Current task state is invalid or corrupted
 */
error_t scheduler_core_get_current_task_safe(tcb_t **task_out);

#endif // SCHEDULER_CORE_H