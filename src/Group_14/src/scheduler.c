#include "scheduler.h"
#include "kmalloc.h"
#include "terminal.h"
#include "string.h"
#include "types.h"

#define TASK_STACK_SIZE 4096

// Global circular run queue and pointer to the currently running task.
static tcb_t *task_list = NULL;
static tcb_t *current_task = NULL;

// Scheduler statistics.
static uint32_t task_count = 0;
static uint32_t context_switches = 0;

/**
 * External assembly routine for context switching.
 * This routine saves the current CPU context (stack pointer) into *old_esp and
 * loads new_esp into ESP, performing the low-level context switch.
 */
extern void context_switch(uint32_t **old_esp, uint32_t *new_esp);

/**
 * task_exit
 *
 * Called if a task function returns. In a real OS, this should remove the task
 * from the scheduler and reclaim resources. Here, we call remove_current_task_with_code
 * and then spin to avoid corrupting memory.
 */
static void task_exit(void) {
    terminal_write("[Scheduler] Task exiting. Removing current task.\n");
    remove_current_task_with_code(0);
    // Should never reach here; spin indefinitely.
    while (1) {
        __asm__ volatile("hlt");
    }
}

/**
 * scheduler_init
 *
 * Initializes the scheduler by clearing the task list and resetting counters.
 */
void scheduler_init(void) {
    task_list = NULL;
    current_task = NULL;
    task_count = 0;
    context_switches = 0;
    terminal_write("[Scheduler] Initialized.\n");
}

/**
 * scheduler_add_task
 *
 * Creates a new TCB with its own kernel stack, sets up the initial call frame
 * (so that when switched in, it calls task_entry and then task_exit if it returns),
 * and adds it to the circular run queue.
 */
void scheduler_add_task(void (*task_entry)(void)) {
    if (!task_entry) {
        terminal_write("[Scheduler] Error: task_entry is NULL.\n");
        return;
    }

    // Allocate TCB.
    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) {
        terminal_write("[Scheduler] Error: Out of memory for TCB.\n");
        return;
    }
    // Allocate kernel stack.
    uint32_t *stack = (uint32_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        terminal_write("[Scheduler] Error: Out of memory for task stack.\n");
        // Optionally free new_task.
        return;
    }
    // Calculate top of the stack (stacks grow downward).
    uint32_t *stack_top = stack + (TASK_STACK_SIZE / sizeof(uint32_t));
    // Set up initial call frame:
    // Push task_exit so that if task_entry returns, it calls task_exit.
    *(--stack_top) = (uint32_t)task_exit;
    // Push task_entry as the initial instruction pointer.
    *(--stack_top) = (uint32_t)task_entry;

    // Initialize TCB.
    new_task->esp = stack_top;
    new_task->pid = (task_list == NULL) ? 1 : (task_count + 1);
    new_task->next = NULL;

    // Insert into circular linked list.
    if (task_list == NULL) {
        task_list = new_task;
        current_task = new_task;
        new_task->next = new_task; // Single task points to itself.
    } else {
        tcb_t *temp = task_list;
        while (temp->next != task_list) {
            temp = temp->next;
        }
        temp->next = new_task;
        new_task->next = task_list;
    }
    task_count++;
    // Log task creation.
    terminal_write("[Scheduler] Added task with PID ");
    // (Assuming terminal_printf is available)
    // terminal_printf("%d\n", new_task->pid);
}

/**
 * yield
 *
 * Voluntary yield: simply calls schedule() to trigger a context switch.
 */
void yield(void) {
    schedule();
}

/**
 * schedule
 *
 * Performs a round-robin context switch.
 * If there's more than one task, switches from current_task to current_task->next.
 */
void schedule(void) {
    if (!current_task || current_task->next == current_task) {
        // Only one task or none; no switch necessary.
        return;
    }
    tcb_t *old_task = current_task;
    tcb_t *new_task = current_task->next;
    current_task = new_task;
    context_switch(&(old_task->esp), new_task->esp);
    context_switches++;
}

/**
 * get_current_task
 *
 * Returns a pointer to the currently running task.
 */
tcb_t *get_current_task(void) {
    return current_task;
}

/**
 * remove_current_task_with_code
 *
 * Removes the current task from the scheduler. This function:
 *   - Logs the removal,
 *   - Adjusts the circular list pointers,
 *   - Frees the task's resources (TCB and stack),
 *   - Schedules the next task.
 *
 * @param code Exit code (can be logged for debugging).
 */
void remove_current_task_with_code(uint32_t code) {
    if (!task_list || !current_task) {
        terminal_write("[Scheduler] No task available to remove.\n");
        return;
    }
    
    // Log removal with PID.
    terminal_write("[Scheduler] Removing task with PID ");
    // (Assuming terminal_printf is available)
    // terminal_printf("%d, exit code: %d\n", current_task->pid, code);

    // If only one task exists, clean up and halt.
    if (current_task->next == current_task) {
        // Optionally free resources here.
        task_list = NULL;
        current_task = NULL;
        task_count = 0;
        terminal_write("[Scheduler] Last task removed. Halting.\n");
        while (1) { __asm__ volatile("hlt"); }
    }
    
    // Find the task preceding current_task.
    tcb_t *prev = task_list;
    while (prev->next != current_task) {
        prev = prev->next;
    }
    // Remove current_task from the list.
    prev->next = current_task->next;
    // If current_task is the head, update task_list.
    if (task_list == current_task) {
        task_list = current_task->next;
    }
    // Save pointer to be removed.
    tcb_t *to_remove = current_task;
    // Advance current_task.
    current_task = current_task->next;
    task_count--;
    
    // Free resources for the removed task.
    // In a real system, you would call kfree on the TCB and its allocated stack.
    // Example:
    // kfree(to_remove->stack_base, TASK_STACK_SIZE);
    // kfree(to_remove, sizeof(tcb_t));
    
    // Perform a context switch to the new current task.
    context_switch(&(to_remove->esp), current_task->esp);
}

/**
 * debug_scheduler_stats
 *
 * Returns the current number of tasks and context switches.
 *
 * @param out_task_count Output pointer for the task count.
 * @param out_switches   Output pointer for context switch count.
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches) {
    if (out_task_count) {
        *out_task_count = task_count;
    }
    if (out_switches) {
        *out_switches = context_switches;
    }
}
