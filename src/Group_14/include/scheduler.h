#pragma once
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h" 

/**
 * @brief Task Control Block (TCB)
 *
 * Each task is represented by a TCB containing a saved stack pointer (esp),
 * a unique process ID (pid), and a pointer to the next TCB in the circular run queue.
 */
typedef struct tcb {
    uint32_t *esp;      ///< Saved stack pointer for context switching.
    uint32_t pid;       ///< Unique process ID.
    struct tcb *next;   ///< Next task in the circular linked list.
} tcb_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the scheduler.
 *
 * Resets internal pointers and counters.
 */
void scheduler_init(void);

/**
 * @brief Adds a new task to the scheduler.
 *
 * Allocates a new TCB and a kernel stack for the task, creates an initial
 * call frame so that the task starts at task_entry, and inserts it into the
 * circular run queue.
 *
 * @param task_entry Pointer to the function where the new task starts.
 */
void scheduler_add_task(void (*task_entry)(void));

/**
 * @brief Voluntarily yields the CPU.
 *
 * Invokes the scheduler to perform a context switch.
 */
void yield(void);

/**
 * @brief Performs round‑robin scheduling.
 *
 * Saves the context of the currently running task and switches to the next one.
 */
void schedule(void);

/**
 * @brief Retrieves the currently running task.
 *
 * @return Pointer to the current task's TCB.
 */
tcb_t *get_current_task(void);

/**
 * @brief Removes the current task from the scheduler.
 *
 * This function removes the currently running task from the run queue,
 * reclaims its resources, and switches to the next task. In a production
 * OS, this function should also free the task's kernel stack and TCB.
 *
 * @param code Exit code of the task (for logging or debugging purposes).
 */
void remove_current_task_with_code(uint32_t code);

/**
 * @brief Retrieves scheduler statistics.
 *
 * Fills in the total number of tasks and context switches.
 *
 * @param[out] out_task_count Pointer to store the task count.
 * @param[out] out_switches Pointer to store the number of context switches.
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULER_H
