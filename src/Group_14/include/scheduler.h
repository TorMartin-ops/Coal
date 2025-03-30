#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <libc/stdint.h>

/* Task Control Block (TCB) structure for each process */
typedef struct tcb {
    uint32_t *esp;        // Saved stack pointer (holds the CPU context)
    struct tcb *next;     // Next task (for round-robin scheduling)
    uint32_t pid;         // Process ID (a unique identifier)
} tcb_t;

/* Initializes the scheduler (must be called early in kernel initialization) */
void scheduler_init(void);

/* Adds a new task to the scheduler.
   task_entry: pointer to the function where the new task begins execution. */
void scheduler_add_task(void (*task_entry)(void));

/* Yields control to the next task (voluntary yield). */
void yield(void);

/* Performs the scheduling, switching from the current task to the next task.
   This is called by yield() and now also from the PIT interrupt for preemption. */
void schedule(void);

#endif // SCHEDULER_H
