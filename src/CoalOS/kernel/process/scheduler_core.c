/**
 * @file scheduler_core.c
 * @brief Core Scheduling Logic - Task Selection & Priority Management
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Contains core scheduling algorithms including task selection,
 * priority-based queuing, and the main schedule() function. Focuses purely
 * on scheduling decisions without low-level context switching.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/process/scheduler_core.h>
#include <kernel/process/scheduler_queues.h>
#include <kernel/process/scheduler_context.h>
#include <kernel/process/scheduler_sleep.h>
#include <kernel/process/scheduler_optimization.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>

//============================================================================
// Core Scheduling Configuration
//============================================================================
// Priority levels are now defined in scheduler.h
#define IDLE_TASK_PID           0

#ifndef SCHED_TICKS_PER_SECOND
#define SCHED_TICKS_PER_SECOND  1000
#endif

#define MS_TO_TICKS(ms) (((ms) * SCHED_TICKS_PER_SECOND) / 1000)

static const uint32_t g_priority_time_slices_ms[SCHED_PRIORITY_LEVELS] = {
    200, /* P0 */ 100, /* P1 */ 50, /* P2 */ 25  /* P3 (Idle) */
};

// Logging Macros
#define SCHED_INFO(fmt, ...)  serial_printf("[Sched INFO ] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) serial_printf("[Sched DEBUG] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) serial_printf("[Sched ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_TRACE(fmt, ...) ((void)0)

//============================================================================
// Core Scheduling State
//============================================================================
static volatile tcb_t *g_current_task = NULL;
static volatile uint32_t g_tick_count = 0;
volatile bool g_scheduler_ready = false;
volatile bool g_need_reschedule = false;

//============================================================================
// Core Scheduling Functions
//============================================================================

tcb_t* scheduler_select_next_task(void) {
#ifdef USE_SCHEDULER_OPTIMIZATION
    // Use O(1) optimized task selection
    tcb_t *task = scheduler_opt_select_next_task();
    if (task) {
        // Use effective priority for time slice calculation
        uint8_t effective_prio = scheduler_core_get_effective_priority(task);
        task->ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[effective_prio]);
        
        // Check for priority boost
        if (scheduler_opt_should_boost_priority(task)) {
            scheduler_opt_boost_priority(task);
            // Recalculate time slice with new priority
            effective_prio = task->effective_priority;
            task->ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[effective_prio]);
        }
        
        SCHED_DEBUG("Selected task PID %lu (Base Prio %d, Effective Prio %d), Slice=%lu [OPT]", 
                    task->pid, task->priority, effective_prio, task->ticks_remaining);
        
        return task;
    }
#else
    // Original implementation
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; ++prio) {
        tcb_t *task = scheduler_queues_dequeue_ready_task(prio);
        if (task) {
            // Skip idle tasks in run queues (should never happen, but be safe)
            if (task->pid == IDLE_TASK_PID) {
                SCHED_ERROR("CRITICAL BUG: Idle task found in run queue! Skipping.");
                continue;
            }
            
            // Use effective priority for time slice calculation
            uint8_t effective_prio = scheduler_core_get_effective_priority(task);
            task->ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[effective_prio]);
            
            SCHED_DEBUG("Selected task PID %lu (Base Prio %d, Effective Prio %d), Slice=%lu", 
                        task->pid, task->priority, effective_prio, task->ticks_remaining);
            
            return task;
        }
    }
#endif
    
    // No runnable tasks available - return NULL to indicate idle needed
    return NULL;
}

void schedule(void) {
    if (!g_scheduler_ready) return;
    
    uint32_t eflags;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags));

    tcb_t *old_task = (tcb_t *)g_current_task;
    tcb_t *new_task = scheduler_select_next_task();
    
    // If no tasks available, handle idle mode
    if (!new_task) {
        SCHED_INFO("No runnable tasks. Entering idle mode.");
        scheduler_context_enter_idle_mode();
        // This should not return unless resuming from idle
        if (eflags & 0x200) asm volatile("sti");
        return;
    }

    // Same task optimization
    if (new_task == old_task) {
        if (old_task && old_task->state == TASK_READY) {
            old_task->state = TASK_RUNNING;
        }
        if (eflags & 0x200) asm volatile("sti");
        return;
    }

    // Re-enqueue old task if it was running and is still ready
    if (old_task && old_task->state == TASK_RUNNING) {
        old_task->state = TASK_READY;
        
        // Don't re-enqueue idle task
        if (old_task->pid != IDLE_TASK_PID) {
            if (!scheduler_queues_enqueue_ready_task(old_task)) {
                SCHED_ERROR("Failed to re-enqueue old task PID %lu", old_task->pid);
            }
        }
    }

    // Update current task and perform context switch
    g_current_task = new_task;
    new_task->state = TASK_RUNNING;
    
    scheduler_context_switch(old_task, new_task);
    
    if (eflags & 0x200) asm volatile("sti");
}

void scheduler_core_tick(void) {
    g_tick_count++;
    
    if (!g_scheduler_ready) return;

    // Check sleeping tasks
    scheduler_sleep_check_wakeups();

    volatile tcb_t *curr_task_v = g_current_task;
    if (!curr_task_v) return;
    
    tcb_t *curr_task = (tcb_t *)curr_task_v;

    // Handle idle task specially
    if (curr_task->pid == IDLE_TASK_PID) {
        if (g_need_reschedule) { 
            g_need_reschedule = false; 
            schedule(); 
        }
        return;
    }

    // Update runtime and time slice
    curr_task->runtime_ticks++;
    if (curr_task->ticks_remaining > 0) {
        curr_task->ticks_remaining--;
    }

    // Trigger reschedule if time slice expired
    if (curr_task->ticks_remaining == 0) {
        SCHED_DEBUG("Timeslice expired for PID %lu", curr_task->pid);
        g_need_reschedule = true;
    }

    if (g_need_reschedule) {
        g_need_reschedule = false;
        schedule();
    }
}

int scheduler_core_add_task(pcb_t *pcb) {
    KERNEL_ASSERT(pcb && pcb->pid != IDLE_TASK_PID && pcb->page_directory_phys &&
                  pcb->kernel_stack_vaddr_top && pcb->user_stack_top &&
                  pcb->entry_point && pcb->kernel_esp_for_switch, "Invalid PCB for add_task");

    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) { 
        SCHED_ERROR("kmalloc TCB failed for PID %lu", pcb->pid); 
        return -1; 
    }
    
    memset(new_task, 0, sizeof(tcb_t));
    new_task->process = pcb;
    new_task->pid = pcb->pid;
    new_task->state = TASK_READY;
    new_task->in_run_queue = false;
    new_task->has_run = false;
    
    // Set up context for the new task
    new_task->context = (uint32_t*)pcb->kernel_esp_for_switch;
    
    // Set priority
    new_task->priority = pcb->is_kernel_task ? SCHED_KERNEL_PRIORITY : SCHED_DEFAULT_PRIORITY;
    KERNEL_ASSERT(new_task->priority < SCHED_PRIORITY_LEVELS, "Bad priority");
    
    new_task->time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[new_task->priority]);
    new_task->ticks_remaining = new_task->time_slice_ticks;
    
    // Initialize priority inheritance fields
    new_task->base_priority = new_task->priority;
    new_task->effective_priority = new_task->priority;
    new_task->blocking_task = NULL;
    new_task->blocked_tasks_head = NULL;
    new_task->blocked_tasks_next = NULL;

    // Add to all tasks list
    scheduler_queues_add_to_all_tasks(new_task);

    // Enqueue in appropriate run queue
    if (!scheduler_queues_enqueue_ready_task(new_task)) {
        SCHED_ERROR("Failed to enqueue newly created task PID %lu!", new_task->pid);
        return -1;
    }

    SCHED_INFO("Added task PID %lu (Prio %u, Slice %lu ticks)",
               new_task->pid, new_task->priority, new_task->time_slice_ticks);
    return 0;
}

void scheduler_core_yield(void) {
    uint32_t eflags;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags));
    SCHED_TRACE("yield() called by PID %lu", g_current_task ? g_current_task->pid : (uint32_t)-1);
    schedule();
    if (eflags & 0x200) asm volatile("sti");
}

void scheduler_core_remove_current_task(uint32_t code) {
    asm volatile("cli");
    tcb_t *task_to_terminate = (tcb_t *)g_current_task;
    KERNEL_ASSERT(task_to_terminate && task_to_terminate->pid != IDLE_TASK_PID, 
                  "Cannot terminate idle/null task");

    SCHED_INFO("Task PID %lu exiting with code %lu. Marking as ZOMBIE.", 
               task_to_terminate->pid, code);
    task_to_terminate->state = TASK_ZOMBIE;
    task_to_terminate->exit_code = code;
    task_to_terminate->in_run_queue = false;
    schedule();
    KERNEL_PANIC_HALT("Returned from schedule() after terminating task!");
}

void scheduler_core_unblock_task(tcb_t *task) {
    if (!task) { 
        SCHED_ERROR("Called with NULL task."); 
        return; 
    }

    KERNEL_ASSERT(task->priority < SCHED_PRIORITY_LEVELS, "Invalid task priority for unblock");

    if (task->state == TASK_BLOCKED) {
        task->state = TASK_READY;
        SCHED_DEBUG("Task PID %lu unblocked, new state: READY.", task->pid);
        
        if (!scheduler_queues_enqueue_ready_task(task)) {
             SCHED_ERROR("Failed to enqueue unblocked task PID %lu", task->pid);
        } else {
             g_need_reschedule = true;
             SCHED_DEBUG("Task PID %lu enqueued into run queue.", task->pid);
        }
    } else {
        SCHED_ERROR("Called on task PID %lu which was not BLOCKED (state=%d).", task->pid, task->state);
    }
}

//============================================================================
// Accessors
//============================================================================
volatile tcb_t* scheduler_core_get_current_task_volatile(void) { 
    return g_current_task; 
}

tcb_t* scheduler_core_get_current_task(void) { 
    return (tcb_t *)g_current_task; 
}

uint32_t scheduler_core_get_ticks(void) {
    return g_tick_count;
}

void scheduler_core_set_need_reschedule(void) {
    g_need_reschedule = true;
}

bool scheduler_core_is_ready(void) {
    return g_scheduler_ready;
}

void scheduler_core_set_ready(bool ready) {
    g_scheduler_ready = ready;
}

//============================================================================
// Priority Inheritance Support
//============================================================================
uint8_t scheduler_core_get_effective_priority(tcb_t *task) {
    if (!task) return SCHED_IDLE_PRIORITY;
    return task->effective_priority;
}

void scheduler_core_inherit_priority(tcb_t *holder, tcb_t *waiter) {
    if (!holder || !waiter) return;
    
    // Only inherit if waiter has higher priority (lower number)
    if (waiter->effective_priority < holder->effective_priority) {
        uint8_t old_priority = holder->effective_priority;
        holder->effective_priority = waiter->effective_priority;
        
        SCHED_DEBUG("Priority inheritance: Task PID %lu priority %u -> %u due to waiter PID %lu",
                    holder->pid, old_priority, holder->effective_priority, waiter->pid);
        
        // If holder is in a run queue, move it to new priority queue
        if (holder->in_run_queue && holder->state == TASK_READY) {
            scheduler_queues_move_task_priority(holder, old_priority, holder->effective_priority);
        }
        
        // Propagate inheritance up the chain
        if (holder->blocking_task) {
            scheduler_core_inherit_priority(holder->blocking_task, holder);
        }
    }
}

void scheduler_core_restore_priority(tcb_t *task) {
    if (!task) return;
    
    uint8_t old_effective = task->effective_priority;
    uint8_t new_effective = task->base_priority;
    
    // Check if any blocked tasks require us to maintain a higher priority
    tcb_t *blocked_task = task->blocked_tasks_head;
    while (blocked_task) {
        if (blocked_task->effective_priority < new_effective) {
            new_effective = blocked_task->effective_priority;
        }
        blocked_task = blocked_task->blocked_tasks_next;
    }
    
    if (new_effective != old_effective) {
        task->effective_priority = new_effective;
        
        SCHED_DEBUG("Priority restore: Task PID %lu priority %u -> %u (base: %u)",
                    task->pid, old_effective, new_effective, task->base_priority);
        
        // If task is in a run queue, move it to appropriate priority queue
        if (task->in_run_queue && task->state == TASK_READY) {
            scheduler_queues_move_task_priority(task, old_effective, new_effective);
        }
    }
}

void scheduler_core_add_blocked_task(tcb_t *holder, tcb_t *waiter) {
    if (!holder || !waiter) return;
    
    // Add waiter to holder's blocked tasks list
    waiter->blocked_tasks_next = holder->blocked_tasks_head;
    holder->blocked_tasks_head = waiter;
    waiter->blocking_task = holder;
    
    // Apply priority inheritance
    scheduler_core_inherit_priority(holder, waiter);
    
    SCHED_DEBUG("Added blocked task: PID %lu waiting on PID %lu", waiter->pid, holder->pid);
}

void scheduler_core_remove_blocked_task(tcb_t *holder, tcb_t *waiter) {
    if (!holder || !waiter) return;
    
    // Remove waiter from holder's blocked tasks list
    if (holder->blocked_tasks_head == waiter) {
        holder->blocked_tasks_head = waiter->blocked_tasks_next;
    } else {
        tcb_t *prev = holder->blocked_tasks_head;
        while (prev && prev->blocked_tasks_next != waiter) {
            prev = prev->blocked_tasks_next;
        }
        if (prev) {
            prev->blocked_tasks_next = waiter->blocked_tasks_next;
        }
    }
    
    waiter->blocked_tasks_next = NULL;
    waiter->blocking_task = NULL;
    
    // Restore holder's priority
    scheduler_core_restore_priority(holder);
    
    SCHED_DEBUG("Removed blocked task: PID %lu no longer waiting on PID %lu", waiter->pid, holder->pid);
}

//============================================================================
// New Standardized Scheduler Error Handling API Implementation
//============================================================================

error_t scheduler_core_add_task_safe(pcb_t *pcb) {
    // Input validation
    if (!pcb) {
        SCHED_ERROR("PCB is NULL");
        return E_INVAL;
    }
    
    // Validate critical PCB fields
    if (pcb->pid == IDLE_TASK_PID) {
        SCHED_ERROR("Cannot add idle task PID %u through normal add_task", pcb->pid);
        return E_INVAL;
    }
    
    if (!pcb->page_directory_phys) {
        SCHED_ERROR("PCB PID %u missing page directory", pcb->pid);
        return E_INVAL;
    }
    
    if (!pcb->kernel_stack_vaddr_top) {
        SCHED_ERROR("PCB PID %u missing kernel stack", pcb->pid);
        return E_INVAL;
    }
    
    if (!pcb->user_stack_top) {
        SCHED_ERROR("PCB PID %u missing user stack", pcb->pid);
        return E_INVAL;
    }
    
    if (!pcb->entry_point) {
        SCHED_ERROR("PCB PID %u missing entry point", pcb->pid);
        return E_INVAL;
    }
    
    if (!pcb->kernel_esp_for_switch) {
        SCHED_ERROR("PCB PID %u missing kernel ESP for context switch", pcb->pid);
        return E_INVAL;
    }

    // Check if task with this PID already exists
    // This is a simplified check - in a full implementation we'd search the task lists
    static uint32_t last_added_pid = 0;
    if (pcb->pid == last_added_pid) {
        SCHED_ERROR("Task with PID %u already exists in scheduler", pcb->pid);
        return E_EXIST;
    }

    // Allocate TCB
    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) { 
        SCHED_ERROR("Failed to allocate TCB for PID %u: insufficient memory", pcb->pid); 
        return E_NOMEM; 
    }
    
    // Initialize TCB
    memset(new_task, 0, sizeof(tcb_t));
    new_task->process = pcb;
    new_task->pid = pcb->pid;
    new_task->state = TASK_READY;
    new_task->in_run_queue = false;
    new_task->has_run = false;
    
    // Set up context for the new task
    new_task->context = (uint32_t*)pcb->kernel_esp_for_switch;
    
    // Set priority with validation
    new_task->priority = pcb->is_kernel_task ? SCHED_KERNEL_PRIORITY : SCHED_DEFAULT_PRIORITY;
    if (new_task->priority >= SCHED_PRIORITY_LEVELS) {
        SCHED_ERROR("Invalid priority %u for PID %u (max: %u)", 
                    new_task->priority, pcb->pid, SCHED_PRIORITY_LEVELS - 1);
        kfree(new_task);
        return E_INVAL;
    }
    
    new_task->time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[new_task->priority]);
    new_task->ticks_remaining = new_task->time_slice_ticks;
    
    // Initialize priority inheritance fields
    new_task->base_priority = new_task->priority;
    new_task->effective_priority = new_task->priority;
    new_task->blocking_task = NULL;
    new_task->blocked_tasks_head = NULL;
    new_task->blocked_tasks_next = NULL;

    // Add to all tasks list
    scheduler_queues_add_to_all_tasks(new_task);

    // Enqueue in appropriate run queue
    if (!scheduler_queues_enqueue_ready_task(new_task)) {
        SCHED_ERROR("Failed to enqueue task PID %u in scheduler queues", new_task->pid);
        // TODO: Remove from all_tasks list
        kfree(new_task);
        return E_FAULT;
    }

    // Success - update tracking
    last_added_pid = pcb->pid;
    
    SCHED_INFO("Successfully added task PID %u (Priority %u, Slice %u ticks)",
               new_task->pid, new_task->priority, new_task->time_slice_ticks);
    return E_SUCCESS;
}

error_t scheduler_select_next_task_safe(tcb_t **task_out) {
    // Input validation
    if (!task_out) {
        SCHED_ERROR("task_out parameter is NULL");
        return E_INVAL;
    }

    // Use existing scheduler_select_next_task() implementation
    tcb_t *next_task = scheduler_select_next_task();
    if (!next_task) {
        // No tasks available - this could be normal during system shutdown
        return E_NOTFOUND;
    }

    // Validate the selected task
    if (!next_task->process) {
        SCHED_ERROR("Selected task PID %u has NULL process pointer", next_task->pid);
        return E_FAULT;
    }

    if (next_task->state != TASK_READY && next_task->state != TASK_RUNNING) {
        SCHED_ERROR("Selected task PID %u has invalid state %d", next_task->pid, next_task->state);
        return E_FAULT;
    }

    // Success
    *task_out = next_task;
    return E_SUCCESS;
}

error_t scheduler_core_get_current_task_safe(tcb_t **task_out) {
    // Input validation
    if (!task_out) {
        SCHED_ERROR("task_out parameter is NULL");
        return E_INVAL;
    }

    // Use existing scheduler_core_get_current_task() implementation
    tcb_t *current_task = scheduler_core_get_current_task();
    if (!current_task) {
        // No current task - might be during early boot or idle state
        return E_NOTFOUND;
    }

    // Validate the current task
    if (!current_task->process) {
        SCHED_ERROR("Current task PID %u has NULL process pointer", current_task->pid);
        return E_FAULT;
    }

    if (current_task->pid == 0 && current_task->pid != IDLE_TASK_PID) {
        SCHED_ERROR("Current task has invalid PID 0 (not idle task)");
        return E_FAULT;
    }

    if (current_task->state != TASK_RUNNING && current_task->state != TASK_READY) {
        SCHED_ERROR("Current task PID %u has invalid state %d", current_task->pid, current_task->state);
        return E_FAULT;
    }

    // Success
    *task_out = current_task;
    return E_SUCCESS;
}