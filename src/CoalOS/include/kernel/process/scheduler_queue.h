/**
 * @file scheduler_queue.h
 * @brief Scheduler Queue Management Interface
 * 
 * Provides functions for managing run queues and sleep queue in the scheduler.
 */

#ifndef KERNEL_PROCESS_SCHEDULER_QUEUE_H
#define KERNEL_PROCESS_SCHEDULER_QUEUE_H

#include <kernel/core/types.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

// Forward declarations
typedef struct tcb tcb_t;
typedef struct run_queue run_queue_t;

// Priority levels (should match scheduler.c)
#ifndef SCHED_PRIORITY_LEVELS
#define SCHED_PRIORITY_LEVELS 4
#endif

/**
 * @brief Initialize a run queue
 * @param queue Queue to initialize
 */
void scheduler_queue_init_run_queue(run_queue_t *queue);

/**
 * @brief Initialize all run queues
 */
void scheduler_queue_init_all_run_queues(void);

/**
 * @brief Initialize the sleep queue
 */
void scheduler_queue_init_sleep_queue(void);

/**
 * @brief Enqueue a task to its priority run queue
 * @param task Task to enqueue
 * @return true on success, false on failure
 * @note Caller must hold scheduler lock
 */
bool scheduler_queue_enqueue_task(tcb_t *task);

/**
 * @brief Dequeue a task from its run queue
 * @param task Task to dequeue
 * @return true on success, false on failure
 * @note Caller must hold scheduler lock
 */
bool scheduler_queue_dequeue_task(tcb_t *task);

/**
 * @brief Add a task to the sleep queue
 * @param task Task to add
 * @note Caller must hold scheduler lock
 */
void scheduler_queue_add_to_sleep_queue(tcb_t *task);

/**
 * @brief Remove a task from the sleep queue
 * @param task Task to remove
 * @note Caller must hold scheduler lock
 */
void scheduler_queue_remove_from_sleep_queue(tcb_t *task);

/**
 * @brief Check sleeping tasks and wake those whose sleep time has expired
 * @param current_ticks Current system tick count
 * @return Number of tasks woken
 * @note Caller must hold scheduler lock
 */
int scheduler_queue_check_sleeping_tasks(uint32_t current_ticks);

/**
 * @brief Find the highest priority task ready to run
 * @return Pointer to selected task, or NULL if no tasks ready
 * @note Caller must hold scheduler lock
 */
tcb_t* scheduler_queue_select_next_task(void);

/**
 * @brief Get the total number of tasks in all run queues
 * @return Total task count
 * @note Caller must hold scheduler lock
 */
uint32_t scheduler_queue_get_ready_task_count(void);

/**
 * @brief Get run queue for a specific priority
 * @param priority Priority level
 * @return Pointer to run queue, or NULL if invalid priority
 */
run_queue_t* scheduler_queue_get_run_queue(uint8_t priority);

/**
 * @brief Get the head of the sleep queue
 * @return Pointer to first sleeping task, or NULL if empty
 */
tcb_t* scheduler_queue_get_sleep_queue_head(void);

#endif // KERNEL_PROCESS_SCHEDULER_QUEUE_H