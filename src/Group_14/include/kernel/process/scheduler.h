// include/scheduler.h (Version 4.0 - For Advanced Scheduler)
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <kernel/process/process.h> // Include process header for pcb_t definition
#include <libc/stdint.h>
#include <libc/stdbool.h> // Ensure bool is included

// --- Enhanced Task States ---
typedef enum {
    TASK_READY,     // Ready to run (in a run queue)
    TASK_RUNNING,   // Currently executing
    TASK_BLOCKED,   // Waiting for an event (in a wait queue, not run queue)
    TASK_SLEEPING,  // Sleeping until a specific time (in the sleep queue)
    TASK_ZOMBIE,    // Terminated, resources awaiting cleanup
    TASK_EXITING    // Intermediate state during termination (optional)
} task_state_e; // Changed name to avoid conflict if task_state_t is used elsewhere

// --- Enhanced Task Control Block (TCB) ---
typedef struct tcb {
    // Core Task Info & Links
    struct tcb    *next;         // Next task in the run queue OR wait queue OR all_tasks list
    pcb_t         *process;      // Pointer to parent process
    uint32_t       pid;          // Process ID

    // Execution Context
    uint32_t      *esp;          // Saved kernel stack pointer

    // State & Scheduling Parameters
    task_state_e   state;        // Current state
    bool           in_run_queue; // <<< ADDED: True if task is currently in a run queue
    bool           has_run;      // True if task has executed at least once
    uint8_t        priority;     // Task priority (0=highest)
    uint32_t       time_slice_ticks; // Current time slice allocation in ticks
    uint32_t       ticks_remaining; // Ticks left in current time slice

    // Statistics & Sleep
    uint32_t       runtime_ticks;  // Total runtime in ticks
    uint32_t       wakeup_time;    // Absolute tick count when to wake up (if SLEEPING)
    uint32_t       exit_code;      // Exit code when ZOMBIE

    // Wait Queue Links (used for BLOCKED state on mutexes, semaphores, etc.)
    struct tcb    *wait_prev;    // Previous in wait list (NULL if first or not waiting)
    struct tcb    *wait_next;    // Next in wait list (NULL if last or not waiting)
    void          *wait_reason;  // Pointer to object being waited on (optional context)

    // All Tasks List Link
    struct tcb    *all_tasks_next; // Next TCB in the global list of all tasks

} tcb_t;


// --- Constants ---
#define IDLE_TASK_PID 0 // Special PID for the idle task

// --- Public Function Prototypes ---

/** @brief Initializes the scheduler subsystem. */
void scheduler_init(void);

/**
 * @brief Creates a TCB for a given process and adds it to the scheduler.
 * @param pcb Pointer to the Process Control Block to schedule.
 * @return 0 on success, negative error code on failure.
 */
int scheduler_add_task(pcb_t *pcb);

/**
 * @brief Core scheduler function. Selects next task, performs context switch.
 * @note Called with interrupts disabled.
 */
void schedule(void);

/** @brief Voluntarily yields the CPU to another task. */
void yield(void);

/**
 * @brief Puts the current task to sleep for a specified duration.
 * @param ms Duration in milliseconds. Task state becomes SLEEPING.
 * @note The task will be woken up by the scheduler_tick handler.
 */
void sleep_ms(uint32_t ms);

/**
 * @brief Marks the current running task as ZOMBIE and triggers a context switch.
 * @param code The exit code for the process.
 * @note This function does not return to the caller.
 */
void remove_current_task_with_code(uint32_t code);

/** @brief Returns a volatile pointer to the currently running task's TCB. */
volatile tcb_t *get_current_task_volatile(void);

/** @brief Returns a non-volatile pointer to the currently running task's TCB. */
tcb_t *get_current_task(void);

/** @brief Frees resources associated with ZOMBIE tasks. */
void scheduler_cleanup_zombies(void);

/** @brief Retrieves basic scheduler statistics. */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches);

/** @brief Checks if the scheduler is ready for preemptive context switching. */
bool scheduler_is_ready(void);

/** @brief Marks the scheduler as ready to perform context switching. */
void scheduler_start(void);

/**
 * @brief Scheduler's timer tick routine.
 * @details Called by the timer interrupt handler. Updates ticks, checks
 * sleeping tasks, manages time slices, and triggers preemption.
 * @note Must be called with interrupts disabled.
 */
void scheduler_tick(void);

/**
 * @brief Returns the current system tick count.
 * @return The volatile tick count.
 */
uint32_t scheduler_get_ticks(void);


// --- External Declarations ---
extern volatile bool g_scheduler_ready;

extern volatile bool g_need_reschedule;

// --- External Assembly Function Prototypes ---
extern void jump_to_user_mode(uint32_t *kernel_stack_ptr, uint32_t *page_directory_phys);
extern void context_switch(uint32_t **old_esp_ptr, uint32_t *new_esp, uint32_t *new_page_directory);

/**
 * @brief Makes a previously blocked task ready and enqueues it.
 * @param task Pointer to the TCB of the task to unblock.
 * @note This function should be called when an event a task was waiting for occurs.
 */
 void scheduler_unblock_task(tcb_t *task);


#endif // SCHEDULER_H