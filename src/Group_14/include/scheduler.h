#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <libc/stdint.h>    // For uint32_t, uintptr_t etc.
#include <libc/stdbool.h>   // For bool type

// Forward declare pcb_t - include process.h if pcb_t is defined there
struct pcb;

// --- Constants and Definitions ---

#define IDLE_TASK_PID 0 // Reserved PID for the idle task

// --- Task States ---
typedef enum {
    TASK_READY,     // Task is ready to run but not currently running
    TASK_RUNNING,   // Task is currently executing on a CPU
    TASK_BLOCKED,   // Task is waiting for an event (e.g., I/O, semaphore) - Placeholder
    TASK_ZOMBIE,    // Task has finished execution but resources not yet fully reclaimed
    TASK_STATE_COUNT // Number of states, useful for validation
} task_state_t;

// --- Task Control Block (TCB) ---
// Contains scheduler-specific information for a task.
typedef struct tcb {
    uint32_t *esp;                 // Saved kernel stack pointer (when not running)
    task_state_t state;            // Current state of the task
    struct tcb *next;              // Next task in the scheduler's circular list
    struct pcb *process;           // Pointer to the associated process control block (contains memory map, etc.)
    uint32_t pid;                  // Process ID (copied from PCB for convenience/logging)
    bool has_run;                  // Flag: true if the task has been context-switched into at least once

    // --- Potential future extensions ---
    // int priority;               // For priority scheduling
    // uint64_t time_slice_start; // For time-slicing
    // uint64_t time_slice_length;
    // struct wait_queue *wq;     // If blocked, which queue it's on
    // uint32_t exit_code;        // Store exit code when becoming ZOMBIE
    // uint32_t cpu_id;           // For SMP affinity

} tcb_t;

// --- Public Scheduler Functions ---

/**
 * @brief Initializes the scheduler subsystem.
 * Must be called once during kernel initialization before adding any tasks.
 * Creates and initializes the mandatory idle task.
 */
void scheduler_init(void);

/**
 * @brief Creates a TCB for a given process and adds it to the scheduler's run queue.
 * Prepares the initial kernel stack for the first transition into the task
 * (either kernel idle task or user mode via IRET).
 *
 * @param pcb Pointer to the fully initialized Process Control Block.
 * @return 0 on success, negative error code on failure (e.g., -1 for no memory).
 */
int scheduler_add_task(struct pcb *pcb);

/**
 * @brief The core scheduling function.
 * Selects the next READY task and performs a context switch.
 * Should only be called when preemption is safe (e.g., from timer interrupt,
 * syscall handler before returning, or yield).
 * Assumes interrupts are disabled by the caller or hardware mechanism.
 */
void schedule(void);

/**
 * @brief Voluntarily yields the CPU to another task.
 * A task calls this to give up its remaining time slice (if applicable)
 * or simply allow other tasks to run.
 */
void yield(void);

/**
 * @brief Gets the TCB of the currently running task.
 * @warning Returns a non-volatile pointer. Use with extreme caution if preemption
 * is enabled, as the current task could change immediately after the call.
 * Best used with interrupts disabled or in non-preemptible contexts.
 * @return Pointer to the current task's TCB, or NULL if called before scheduling starts.
 */
tcb_t *get_current_task(void);

/**
 * @brief Gets the TCB of the currently running task as a volatile pointer.
 * Safer for read-only access where the possibility of the task changing
 * immediately after the read is acceptable.
 * @return Volatile pointer to the current task's TCB, or NULL if called before scheduling starts.
 */
volatile tcb_t *get_current_task_volatile(void);

/**
 * @brief Marks the currently running task as ZOMBIE and schedules another task.
 * This initiates the termination sequence for the current task. Resource cleanup
 * is deferred to scheduler_cleanup_zombies().
 * This function does not return to the caller in the context of the terminated task.
 *
 * @param code The exit code (currently used only for logging).
 * @note Must be called with interrupts disabled or from a non-preemptible context.
 */
void remove_current_task_with_code(uint32_t code);

/**
 * @brief Cleans up resources associated with ZOMBIE tasks.
 * Iterates through the task list, finds tasks marked as ZOMBIE, removes them
 * from the list, and frees their associated PCB and TCB resources.
 * Should be called periodically (e.g., by the idle task or a dedicated reaper task).
 */
void scheduler_cleanup_zombies(void);

/**
 * @brief Retrieves basic scheduler statistics.
 * @param out_task_count Pointer to store the current total number of tasks (including idle). Can be NULL.
 * @param out_switches Pointer to store the total number of context switches performed. Can be NULL.
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches);

/**
 * @brief Checks if the scheduler is ready for preemptive operation.
 * Typically set true by the timer interrupt initialization.
 * @return True if the scheduler is ready, false otherwise.
 */
bool scheduler_is_ready(void);


// --- External Assembly Function Declarations ---
// (Place these here or in a dedicated arch/asm header)

/**
 * @brief Performs a context switch between two kernel contexts.
 * Saves the state of the task corresponding to old_esp_ptr (if not NULL)
 * onto its kernel stack, loads the state from new_esp, and potentially
 * switches the page directory (CR3) if new_page_directory_phys is non-NULL
 * and different from the current CR3. Returns execution in the new context.
 *
 * @param old_esp_ptr Address where the old task's final ESP should be saved.
 * @param new_esp The kernel stack pointer for the task to switch to.
 * @param new_page_directory_phys Physical address of the page directory for the new task,
 * or NULL if the page directory should not be switched.
 */
extern void context_switch(uint32_t **old_esp_ptr,
                           uint32_t *new_esp,
                           uint32_t *new_page_directory_phys);

/**
 * @brief Performs the initial jump from kernel mode to user mode for a new task.
 * Loads the specified page directory, sets up the kernel stack pointer (which
 * should point to a pre-prepared IRET frame), and executes IRET.
 * This function does not save any prior state and does not return.
 *
 * @param kernel_stack_ptr Pointer to the top of the kernel stack containing the IRET frame
 * (User EIP, CS, EFLAGS, ESP, SS).
 * @param page_directory_phys Physical address of the user process's page directory.
 */
extern void jump_to_user_mode(uint32_t *kernel_stack_ptr,
                              uint32_t *page_directory_phys);


#endif // SCHEDULER_H