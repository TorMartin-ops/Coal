/**
 * @file scheduler_queues.c
 * @brief Queue Management for Scheduler - Run Queues & Task Lists
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Responsible for managing run queues, task enqueue/dequeue operations,
 * and maintaining the global list of all tasks. Focuses purely on queue
 * data structure management.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/process/scheduler_queues.h>
#include <kernel/process/scheduler_optimization.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/string.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Queue Configuration
//============================================================================
// SCHED_PRIORITY_LEVELS is now defined in scheduler.h
#define IDLE_TASK_PID           0

// Logging Macros
#define SCHED_ERROR(fmt, ...) serial_printf("[Queue ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_WARN(fmt, ...)  serial_printf("[Queue WARN ] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) serial_printf("[Queue DEBUG] " fmt "\n", ##__VA_ARGS__)

//============================================================================
// Queue Data Structures
//============================================================================
typedef struct {
    tcb_t      *head;
    tcb_t      *tail;
    uint32_t    count;
    spinlock_t  lock;
} run_queue_t;

//============================================================================
// Module Static Data
//============================================================================
static run_queue_t g_run_queues[SCHED_PRIORITY_LEVELS];
static tcb_t *g_all_tasks_head = NULL;
static spinlock_t g_all_tasks_lock;

//============================================================================
// Queue Management Implementation
//============================================================================

void scheduler_queues_init(void) {
    // Initialize run queues
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        g_run_queues[i].head = NULL;
        g_run_queues[i].tail = NULL;
        g_run_queues[i].count = 0;
        spinlock_init(&g_run_queues[i].lock);
    }
    
    // Initialize all tasks list
    g_all_tasks_head = NULL;
    spinlock_init(&g_all_tasks_lock);
    
    SCHED_DEBUG("Queue management initialized with %d priority levels", SCHED_PRIORITY_LEVELS);
}

static bool enqueue_task_locked(run_queue_t *queue, tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot enqueue NULL task");
    KERNEL_ASSERT(task->state == TASK_READY, "Enqueueing task that is not READY");

    // CRITICAL: Never enqueue the idle task
    if (task->pid == IDLE_TASK_PID) {
        SCHED_ERROR("CRITICAL BUG: Attempted to enqueue idle task PID %lu!", task->pid);
        return false;
    }

    if (task->in_run_queue) {
        SCHED_WARN("Task PID %lu already marked as in_run_queue during enqueue attempt.", task->pid);
        return false;
    }

    task->next = NULL;

    if (queue->tail) {
        queue->tail->next = task;
        queue->tail = task;
    } else {
        KERNEL_ASSERT(queue->head == NULL && queue->count == 0, 
                      "Queue tail is NULL but head isn't or count isn't 0");
        queue->head = task;
        queue->tail = task;
    }
    
    queue->count++;
    task->in_run_queue = true;
    return true;
}

static bool dequeue_task_locked(run_queue_t *queue, tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot dequeue NULL task");

    if (!queue->head) {
        SCHED_WARN("Attempted dequeue from empty queue for task PID %lu", task->pid);
        task->in_run_queue = false;
        return false;
    }

    if (queue->head == task) {
        queue->head = task->next;
        if (queue->tail == task) { 
            queue->tail = NULL; 
            KERNEL_ASSERT(queue->head == NULL, "Head non-NULL when tail dequeued");
        }
        KERNEL_ASSERT(queue->count > 0, "Queue count underflow (head dequeue)");
        queue->count--;
        task->next = NULL;
        task->in_run_queue = false;
        return true;
    }

    tcb_t *prev = queue->head;
    while (prev->next && prev->next != task) {
        prev = prev->next;
    }

    if (prev->next == task) {
        prev->next = task->next;
        if (queue->tail == task) { 
            queue->tail = prev; 
        }
        KERNEL_ASSERT(queue->count > 0, "Queue count underflow (mid/tail dequeue)");
        queue->count--;
        task->next = NULL;
        task->in_run_queue = false;
        return true;
    }

    SCHED_ERROR("Task PID %lu not found in queue for dequeue!", task->pid);
    task->in_run_queue = false; // Clear flag defensively
    return false;
}

bool scheduler_queues_enqueue_ready_task(tcb_t *task) {
    if (!task || task->priority >= SCHED_PRIORITY_LEVELS) {
        SCHED_ERROR("Invalid task or priority for enqueue");
        return false;
    }

    run_queue_t *queue = &g_run_queues[task->priority];
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
    bool result = enqueue_task_locked(queue, task);
    spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
    
    if (result) {
        SCHED_DEBUG("Enqueued task PID %lu into priority %u queue", task->pid, task->priority);
#ifdef USE_SCHEDULER_OPTIMIZATION
        // Notify optimization module that this priority level has tasks
        scheduler_opt_mark_priority_active(task->priority);
#endif
    }
    
    return result;
}

tcb_t* scheduler_queues_dequeue_ready_task(uint8_t priority) {
    if (priority >= SCHED_PRIORITY_LEVELS) {
        return NULL;
    }

    run_queue_t *queue = &g_run_queues[priority];
    if (!queue->head) {
        return NULL;
    }

    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
    
    // Skip any idle tasks (should never happen, but be safe)
    tcb_t *task = queue->head;
    while (task && task->pid == IDLE_TASK_PID) {
        SCHED_ERROR("CRITICAL BUG: Idle task found in run queue! Removing it.");
        bool dequeued = dequeue_task_locked(queue, task);
        if (!dequeued) break;
        task = queue->head; // Get next task after dequeue
    }
    
    if (task) {
        bool dequeued = dequeue_task_locked(queue, task);
        spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
        
        if (!dequeued) { 
            SCHED_ERROR("Selected task PID %lu Prio %d but failed to dequeue!", task->pid, priority); 
            return NULL;
        }
        
        SCHED_DEBUG("Dequeued task PID %lu from priority %u queue", task->pid, priority);
        return task;
    }
    
    spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
    return NULL;
}

bool scheduler_queues_remove_task(tcb_t *task) {
    if (!task || task->priority >= SCHED_PRIORITY_LEVELS) {
        return false;
    }

    run_queue_t *queue = &g_run_queues[task->priority];
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
    bool result = dequeue_task_locked(queue, task);
    spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
    
    return result;
}

void scheduler_queues_move_task_priority(tcb_t *task, uint8_t old_priority, uint8_t new_priority) {
    if (!task || old_priority >= SCHED_PRIORITY_LEVELS || new_priority >= SCHED_PRIORITY_LEVELS) {
        SCHED_ERROR("Invalid parameters for priority move");
        return;
    }

    if (old_priority == new_priority) {
        return; // No move needed
    }

    // Remove from old priority queue
    run_queue_t *old_queue = &g_run_queues[old_priority];
    uintptr_t old_flags = spinlock_acquire_irqsave(&old_queue->lock);
    bool removed = dequeue_task_locked(old_queue, task);
    spinlock_release_irqrestore(&old_queue->lock, old_flags);
    
    if (!removed) {
        SCHED_ERROR("Failed to remove task PID %lu from priority %u queue", task->pid, old_priority);
        return;
    }

    // Add to new priority queue
    run_queue_t *new_queue = &g_run_queues[new_priority];
    uintptr_t new_flags = spinlock_acquire_irqsave(&new_queue->lock);
    bool added = enqueue_task_locked(new_queue, task);
    spinlock_release_irqrestore(&new_queue->lock, new_flags);
    
    if (!added) {
        SCHED_ERROR("Failed to add task PID %lu to priority %u queue", task->pid, new_priority);
    } else {
        SCHED_DEBUG("Moved task PID %lu from priority %u to %u", task->pid, old_priority, new_priority);
    }
}

void scheduler_queues_add_to_all_tasks(tcb_t *task) {
    if (!task) {
        return;
    }

    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    task->all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = task;
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);
    
    SCHED_DEBUG("Added task PID %lu to all tasks list", task->pid);
}

tcb_t* scheduler_queues_remove_zombie_task(void) {
    tcb_t *zombie_to_reap = NULL;
    tcb_t *prev_all = NULL;

    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    tcb_t *current_all = g_all_tasks_head;
    
    while (current_all) {
        if (current_all->pid != IDLE_TASK_PID && current_all->state == TASK_ZOMBIE) {
            zombie_to_reap = current_all;
            if (prev_all) {
                prev_all->all_tasks_next = current_all->all_tasks_next;
            } else {
                g_all_tasks_head = current_all->all_tasks_next;
            }
            zombie_to_reap->all_tasks_next = NULL;
            break;
        }
        prev_all = current_all;
        current_all = current_all->all_tasks_next;
    }
    
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);
    
    if (zombie_to_reap) {
        SCHED_DEBUG("Found zombie task PID %lu for cleanup", zombie_to_reap->pid);
    }
    
    return zombie_to_reap;
}

//============================================================================
// Queue Statistics & Debug
//============================================================================

uint32_t scheduler_queues_get_count(uint8_t priority) {
    if (priority >= SCHED_PRIORITY_LEVELS) {
        return 0;
    }

    run_queue_t *queue = &g_run_queues[priority];
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
    uint32_t count = queue->count;
    spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
    
    return count;
}

void scheduler_queues_debug_print_stats(void) {
    serial_printf("[Queue Stats] Priority queue counts:\n");
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        uint32_t count = scheduler_queues_get_count(i);
        serial_printf("  Priority %d: %lu tasks\n", i, count);
    }
}