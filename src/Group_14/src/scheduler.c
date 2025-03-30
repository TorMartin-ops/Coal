#include "scheduler.h"
#include <libc/stddef.h>
#include "mem.h"

/* Define the size of the stack for each task (e.g., 4 KB) */
#define TASK_STACK_SIZE 4096

/* Global pointers to maintain the circular task list and the current task */
static tcb_t *current_task = 0;
static tcb_t *task_list = 0;

/* 
 * External declaration of the context switch routine.
 * This routine is implemented in assembly in context_switch.asm.
 * It takes two arguments:
 *   - A pointer to the pointer where the current task’s ESP will be stored.
 *   - The new task’s saved ESP to switch to.
 */
extern void context_switch(uint32_t **old_esp, uint32_t *new_esp);

/* Internal helper: called when a task function returns.
   In a production OS you might remove the task from the list.
   Here we simply loop forever and yield repeatedly. */
static void task_exit(void) {
    while (1) {
        yield();
    }
}

/* Initializes the scheduler by resetting the global task pointers. */
void scheduler_init(void) {
    current_task = 0;
    task_list = 0;
}

/* Adds a new task to the scheduler.
   Allocates a TCB and its kernel stack; initializes the stack frame so that
   when the task is switched in, it starts at task_entry. */
void scheduler_add_task(void (*task_entry)(void)) {
    /* Allocate a new TCB */
    tcb_t *new_task = (tcb_t*)malloc(sizeof(tcb_t));
    if (!new_task) {
        // In production, handle allocation failure appropriately.
        return;
    }
    
    /* Allocate a stack for the new task */
    uint32_t *stack = (uint32_t*)malloc(TASK_STACK_SIZE);
    if (!stack) {
        // In production, handle allocation failure appropriately.
        return;
    }
    /* Calculate the top of the stack (stack grows downward) */
    uint32_t *stack_top = stack + (TASK_STACK_SIZE / sizeof(uint32_t));
    
    /* Set up the initial stack frame:
       We simulate a call frame so that when the task's context is restored,
       a "ret" transfers control to the task_entry function.
       We push task_exit as the return address if task_entry ever returns. */
    *(--stack_top) = (uint32_t)task_exit;   // Return address if task_entry returns
    *(--stack_top) = (uint32_t)task_entry;    // This will be our initial EIP

    /* Initialize the TCB */
    new_task->esp = stack_top;
    new_task->pid = (task_list == 0) ? 1 : task_list->pid + 1;
    new_task->next = 0;

    /* Insert the new task into a circular linked list (round-robin) */
    if (!task_list) {
        task_list = new_task;
        current_task = new_task;
        new_task->next = new_task; // Only task points to itself
    } else {
        tcb_t *temp = task_list;
        while (temp->next != task_list) {
            temp = temp->next;
        }
        temp->next = new_task;
        new_task->next = task_list;
    }
}

/* Voluntary yield: calls schedule() to switch to the next task. */
void yield(void) {
    schedule();
}

/* The scheduler: switches from the current task to the next one.
   It calls the context_switch assembly routine to save and restore CPU state. */
void schedule(void) {
    if (!current_task || current_task->next == current_task)
        return;  // If there's only one task, nothing to do.

    tcb_t *old_task = current_task;
    tcb_t *new_task = current_task->next;
    current_task = new_task;
    
    /* Perform the context switch:
       Save the current task's ESP and load the new task's ESP.
       The assembly routine takes care of saving/restoring registers. */
    context_switch(&(old_task->esp), new_task->esp);
}
