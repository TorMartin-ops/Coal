#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h" // Include process header for pcb_t definition
#include <libc/stdint.h>
#include <libc/stdbool.h> // Ensure bool is included

// --- Task States ---
typedef enum {
    TASK_READY,     // Ready to run
    TASK_RUNNING,   // Currently executing
    TASK_BLOCKED,   // Waiting for an event (e.g., I/O, semaphore)
    TASK_ZOMBIE     // Terminated, waiting for cleanup
} task_state_t;

// --- Task Control Block (TCB) ---
typedef struct tcb {
    pcb_t         *process;      // Pointer to the associated Process Control Block
    uint32_t       pid;          // Process ID (redundant, but useful for quick access)
    task_state_t   state;        // Current state of the task
    uint32_t      *esp;          // Saved kernel stack pointer for context switch/IRET
    struct tcb    *next;         // Pointer to the next TCB in the scheduling list
    bool           has_run;      // Flag: true if task has executed at least once (used for IRET vs context_switch)
    // Add other scheduling info as needed (priority, time slice, etc.)
    // uint32_t       exit_code;    // To store exit code when task becomes ZOMBIE (optional)
} tcb_t;

// --- Constants ---
#define IDLE_TASK_PID 0 // Special PID for the idle task

// --- Public Function Prototypes ---

/**
 * @brief Initializes the scheduler subsystem.
 */
void scheduler_init(void);

/**
 * @brief Creates a TCB for a given process and adds it to the scheduler's run queue.
 * @param pcb Pointer to the Process Control Block to schedule.
 * @return 0 on success, negative error code on failure.
 */
int scheduler_add_task(pcb_t *pcb);

/**
 * @brief The core scheduler function. Selects the next task and performs a context switch.
 * Should be called periodically (e.g., by timer interrupt) or when a task yields/blocks.
 * @note Should be called with interrupts disabled.
 */
void schedule(void);

/**
 * @brief Voluntarily yields the CPU to another task.
 */
void yield(void);

/**
 * @brief Marks the current running task as ZOMBIE (terminated) and triggers a context switch.
 * The task's resources will be cleaned up later by scheduler_cleanup_zombies().
 * @param code The exit code for the process (currently unused but good practice).
 * @note This function should not return to the caller.
 */
void remove_current_task_with_code(uint32_t code);

/**
 * @brief Returns a volatile pointer to the currently running task's TCB.
 * Useful for quick checks, but be wary of race conditions without locks.
 * @return Volatile pointer to the current TCB, or NULL if scheduling hasn't started.
 */
volatile tcb_t *get_current_task_volatile(void);

/**
* @brief Returns a non-volatile pointer to the currently running task's TCB.
* @return Pointer to the current TCB, or NULL if scheduling hasn't started.
* @note Caller must ensure atomicity (e.g., disable interrupts or hold lock) if needed.
*/
tcb_t *get_current_task(void);


/**
 * @brief Frees resources associated with ZOMBIE tasks.
 * Should be called periodically (e.g., by the idle task).
 */
void scheduler_cleanup_zombies(void);

/**
 * @brief Retrieves basic scheduler statistics.
 * @param out_task_count Pointer to store the total number of tasks (including idle).
 * @param out_switches Pointer to store the total number of context switches performed.
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches);


/**
 * @brief Checks if the scheduler is ready for preemptive context switching.
 * @return True if ready, false otherwise.
 */
bool scheduler_is_ready(void);

/**
 * @brief Marks the scheduler as ready to perform preemptive context switching.
 * Should be called after initialization and adding the first task(s), before enabling interrupts.
 */
void scheduler_start(void);


// --- External Declarations ---

// The globally accessible flag indicating if the scheduler is active.
// Set by scheduler_start(), checked by schedule().
extern volatile bool g_scheduler_ready;


#endif // SCHEDULER_H