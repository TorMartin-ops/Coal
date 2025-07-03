/**
 * @file scheduler_context.h
 * @brief Context Switching Interface
 * @author Refactored for SOLID principles
 * @version 6.0
 */

#ifndef SCHEDULER_CONTEXT_H
#define SCHEDULER_CONTEXT_H

#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Context Switching Functions
//============================================================================

/**
 * @brief Initialize the idle task for context switching
 */
void scheduler_context_init_idle_task(void);

/**
 * @brief Perform low-level context switch between tasks
 * @param old_task Previous task (can be NULL)
 * @param new_task New task to switch to
 */
void scheduler_context_switch(tcb_t *old_task, tcb_t *new_task);

/**
 * @brief Enter idle mode directly without context switching
 * @note This function does not return
 */
void scheduler_context_enter_idle_mode(void) __attribute__((noreturn));

//============================================================================
// Idle Task Management
//============================================================================

/**
 * @brief Get pointer to the idle task TCB
 * @return Pointer to idle task
 */
tcb_t* scheduler_context_get_idle_task(void);

/**
 * @brief Check idle task stack integrity
 * @param checkpoint Description of checkpoint for debugging
 */
void scheduler_context_check_idle_integrity(const char *checkpoint);

#endif // SCHEDULER_CONTEXT_H