#pragma once
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"
#include "process.h" // Include process.h for pcb_t definition

/**
 * @brief Task Control Block (TCB)
 *
 * Represents the schedulable entity (often a thread or lightweight process).
 * Contains the saved kernel stack pointer for context switching and a pointer
 * to the associated process's PCB (which holds the address space information).
 */
typedef struct tcb {
    uint32_t *esp;      ///< Saved kernel stack pointer for context switching.
    pcb_t *process;     ///< Pointer to the associated Process Control Block (PCB).
    struct tcb *next;   ///< Next task in the circular run queue.
    // Add other scheduling info: priority, state (ready, running, blocked), time slice etc.
} tcb_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the scheduler.
 */
void scheduler_init(void);

/**
 * @brief Adds a new task to the scheduler, associated with a given process (PCB).
 *
 * Creates a TCB for the given PCB, allocates a kernel stack, sets up the initial
 * stack frame to enable jumping to the process's user-mode entry point on the
 * first context switch, and adds the TCB to the run queue.
 *
 * @param pcb Pointer to the Process Control Block (pcb_t) for the task.
 * @return 0 on success, negative error code on failure.
 */
int scheduler_add_task(pcb_t *pcb);

/**
 * @brief Voluntarily yields the CPU to the next task.
 */
void yield(void);

/**
 * @brief Performs round‑robin scheduling, switching context and address space.
 */
void schedule(void);

/**
 * @brief Gets the TCB of the currently running task.
 * @return Pointer to the current task's TCB, or NULL if none running.
 */
tcb_t *get_current_task(void);

/**
 * @brief Removes the currently running task from the scheduler and frees its resources.
 *
 * Removes the current TCB from the run queue, calls destroy_process() on the
 * associated PCB (which frees page dir, kernel stack, PCB), frees the TCB,
 * and schedules the next available task.
 *
 * @param code Exit code (for logging or potential cleanup).
 */
void remove_current_task_with_code(uint32_t code);

/**
 * @brief Retrieves scheduler statistics (optional).
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULER_H