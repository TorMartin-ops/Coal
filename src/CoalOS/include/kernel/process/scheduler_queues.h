/**
 * @file scheduler_queues.h
 * @brief Queue Management Interface for Scheduler
 * @author Refactored for SOLID principles
 * @version 6.0
 */

#ifndef SCHEDULER_QUEUES_H
#define SCHEDULER_QUEUES_H

#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Queue Management Functions
//============================================================================

/**
 * @brief Initialize all scheduler queues
 */
void scheduler_queues_init(void);

/**
 * @brief Enqueue a ready task into appropriate priority queue
 * @param task Task to enqueue
 * @return True on success, false on failure
 */
bool scheduler_queues_enqueue_ready_task(tcb_t *task);

/**
 * @brief Dequeue the next ready task from a priority queue
 * @param priority Priority level to dequeue from
 * @return Task to run, or NULL if queue is empty
 */
tcb_t* scheduler_queues_dequeue_ready_task(uint8_t priority);

/**
 * @brief Remove a specific task from its queue
 * @param task Task to remove
 * @return True on success, false if not found
 */
bool scheduler_queues_remove_task(tcb_t *task);

/**
 * @brief Move a task between priority queues
 * @param task Task to move
 * @param old_priority Current priority queue
 * @param new_priority Target priority queue
 */
void scheduler_queues_move_task_priority(tcb_t *task, uint8_t old_priority, uint8_t new_priority);

/**
 * @brief Add task to global all tasks list
 * @param task Task to add
 */
void scheduler_queues_add_to_all_tasks(tcb_t *task);

/**
 * @brief Find and remove a zombie task for cleanup
 * @return Zombie task to clean up, or NULL if none found
 */
tcb_t* scheduler_queues_remove_zombie_task(void);

//============================================================================
// Queue Statistics & Debug
//============================================================================

/**
 * @brief Get number of tasks in a priority queue
 * @param priority Priority level to check
 * @return Number of tasks in queue
 */
uint32_t scheduler_queues_get_count(uint8_t priority);

/**
 * @brief Print debug statistics for all queues
 */
void scheduler_queues_debug_print_stats(void);

#endif // SCHEDULER_QUEUES_H