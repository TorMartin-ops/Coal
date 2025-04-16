#pragma once
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"
#include "process.h" // Include process.h for pcb_t definition

// --- Added Task State Enum ---
// Defines the possible states a task can be in.
typedef enum {
    TASK_INVALID = 0, // Should not happen
    TASK_READY,       // Ready to run, in the run queue
    TASK_RUNNING,     // Currently executing on a CPU
    TASK_BLOCKED,     // Waiting for an event (e.g., I/O, semaphore) - Placeholder
    TASK_ZOMBIE       // Terminated, waiting for cleanup by reaper/idle task
} task_state_t;
// --- End Added Enum ---


/**
 * @brief Task Control Block (TCB) - IMPROVED
 *
 * Represents the schedulable entity (often a thread or lightweight process).
 * Contains saved kernel context, link to process, scheduling state, and list pointer.
 */
typedef struct tcb {
    // Core Context Switching Info
    uint32_t *esp;          ///< Saved kernel stack pointer for context switching.

    // Process Association
    pcb_t *process;         ///< Pointer to the associated Process Control Block (PCB).

    // --- Added Fields for Improved Scheduler ---
    uint32_t pid;           ///< Process ID (copied from PCB for convenience).
    volatile task_state_t state; ///< Current state of the task (TASK_READY, etc.). Volatile as it can change unexpectedly.
    // Add other scheduling info if needed: priority, time_slice_remaining, blocked_on_queue, etc.
    // --- End Added Fields ---

    // Linking
    struct tcb *next;       ///< Next task in the circular list (contains tasks in *all* states).

} tcb_t;


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the scheduler. Must be called once.
 */
void scheduler_init(void);

/**
 * @brief Adds a new task to the scheduler, associated with a given process (PCB).
 *
 * Creates a TCB, sets up the initial kernel stack for user mode entry,
 * marks the task as TASK_READY, and adds it to the scheduler's list.
 *
 * @param pcb Pointer to the Process Control Block (pcb_t) for the task.
 * @return 0 on success, negative error code on failure.
 */
int scheduler_add_task(pcb_t *pcb);

/**
 * @brief Voluntarily yields the CPU to the next ready task.
 * If no other task is ready, the current task may continue running.
 */
void yield(void);

/**
 * @brief Core scheduling function (usually called by timer IRQ or yield).
 * Selects the next READY task and performs a context switch if necessary.
 * MUST be called with interrupts disabled or from an interrupt context.
 */
void schedule(void);

/**
 * @brief Gets the TCB of the currently running task.
 * @return Pointer to the current task's TCB, or NULL if kernel is idle/initializing.
 */
tcb_t *get_current_task(void);

/**
 * @brief Marks the currently running task for termination (exit).
 *
 * Sets the task's state to TASK_ZOMBIE and calls schedule() to switch away.
 * The actual cleanup (freeing PCB, TCB, kernel stack) is deferred
 * to a separate mechanism (e.g., an idle task or reaper calling
 * scheduler_cleanup_zombies). This function DOES NOT RETURN to the caller task.
 *
 * @param code Exit code (currently logged but not stored persistently).
 */
void remove_current_task_with_code(uint32_t code);

/**
 * @brief Cleans up resources associated with ZOMBIE tasks. (Example)
 * Needs to be called periodically (e.g., by idle task) to prevent resource leaks.
 * Finds tasks in TASK_ZOMBIE state, calls destroy_process, frees the TCB,
 * and removes them from the scheduler list.
 */
void scheduler_cleanup_zombies(void); // Added declaration

/**
 * @brief Retrieves scheduler statistics (optional).
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULER_H