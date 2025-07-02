/**
 * @file scheduler_task.h
 * @brief Scheduler Task Management Interface
 * 
 * Provides functions for task lifecycle management in the scheduler.
 */

#ifndef KERNEL_PROCESS_SCHEDULER_TASK_H
#define KERNEL_PROCESS_SCHEDULER_TASK_H

#include <kernel/core/types.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

// Forward declarations
typedef struct tcb tcb_t;
typedef struct pcb pcb_t;

// Task states
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

// Default priority level
#ifndef SCHED_DEFAULT_PRIORITY
#define SCHED_DEFAULT_PRIORITY 1
#endif

// Priority levels
#ifndef SCHED_PRIORITY_LEVELS
#define SCHED_PRIORITY_LEVELS 4
#endif

/**
 * @brief Create a TCB from a PCB
 * @param pcb Process control block
 * @return Pointer to new TCB, or NULL on failure
 */
tcb_t* scheduler_task_create_tcb(pcb_t *pcb);

/**
 * @brief Destroy a TCB and free its resources
 * @param tcb Task control block to destroy
 */
void scheduler_task_destroy_tcb(tcb_t *tcb);

/**
 * @brief Add a task to the zombie list
 * @param task Task to add
 * @note Caller must hold appropriate locks
 */
void scheduler_task_add_to_zombie_list(tcb_t *task);

/**
 * @brief Clean up zombie tasks by destroying their PCBs and TCBs
 * @return Number of zombies cleaned up
 */
int scheduler_task_cleanup_zombies(void);

/**
 * @brief Update task state
 * @param task Task to update
 * @param new_state New state
 */
void scheduler_task_set_state(tcb_t *task, task_state_t new_state);

/**
 * @brief Set task priority
 * @param task Task to update
 * @param priority New priority (0 = highest)
 * @return true if priority was valid and set, false otherwise
 */
bool scheduler_task_set_priority(tcb_t *task, uint8_t priority);

/**
 * @brief Check if a task is the idle task
 * @param task Task to check
 * @return true if task is the idle task
 */
bool scheduler_task_is_idle(tcb_t *task);

/**
 * @brief Get the total number of zombies awaiting cleanup
 * @return Zombie count
 */
uint32_t scheduler_task_get_zombie_count(void);

/**
 * @brief Initialize task subsystem
 */
void scheduler_task_init(void);

#endif // KERNEL_PROCESS_SCHEDULER_TASK_H