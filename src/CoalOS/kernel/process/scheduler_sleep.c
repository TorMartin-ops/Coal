/**
 * @file scheduler_sleep.c
 * @brief Sleep Queue Management and Timer-based Task Wakeup
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Manages sleeping tasks, wakeup times, and the sleep queue.
 * Handles time-based task suspension and resumption. Focuses purely
 * on sleep/wake functionality.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/process/scheduler_sleep.h>
#include <kernel/process/scheduler_queues.h>
#include <kernel/process/scheduler_core.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>
#include <libc/limits.h>

//============================================================================
// Sleep Management Configuration
//============================================================================
#define IDLE_TASK_PID 0

#ifndef SCHED_TICKS_PER_SECOND
#define SCHED_TICKS_PER_SECOND  1000
#endif

#define MS_TO_TICKS(ms) (((ms) * SCHED_TICKS_PER_SECOND) / 1000)

// Logging Macros
#define SCHED_DEBUG(fmt, ...) serial_printf("[Sleep DEBUG] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) serial_printf("[Sleep ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_WARN(fmt, ...)  serial_printf("[Sleep WARN ] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Sleep Queue Data Structure
//============================================================================
typedef struct {
    tcb_t      *head;
    uint32_t    count;
    spinlock_t  lock;
} sleep_queue_t;

//============================================================================
// Module Static Data
//============================================================================
static sleep_queue_t g_sleep_queue;

//============================================================================
// Sleep Queue Management
//============================================================================

void scheduler_sleep_init(void) {
    g_sleep_queue.head = NULL;
    g_sleep_queue.count = 0;
    spinlock_init(&g_sleep_queue.lock);
    
    SCHED_DEBUG("Sleep queue initialized");
}

static void add_to_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL && task->state == TASK_SLEEPING, 
                  "Invalid task/state for sleep queue add");
    KERNEL_ASSERT(!task->in_run_queue, "Sleeping task should not be marked as in_run_queue");

    task->wait_next = NULL;
    task->wait_prev = NULL;

    // Insert in sorted order by wakeup time (earliest first)
    if (!g_sleep_queue.head || task->wakeup_time <= g_sleep_queue.head->wakeup_time) {
        task->wait_next = g_sleep_queue.head;
        if (g_sleep_queue.head) {
            g_sleep_queue.head->wait_prev = task;
        }
        g_sleep_queue.head = task;
    } else {
        tcb_t *current = g_sleep_queue.head;
        while (current->wait_next && current->wait_next->wakeup_time <= task->wakeup_time) {
            current = current->wait_next;
        }
        task->wait_next = current->wait_next;
        task->wait_prev = current;
        if (current->wait_next) {
            current->wait_next->wait_prev = task;
        }
        current->wait_next = task;
    }
    
    g_sleep_queue.count++;
    SCHED_DEBUG("Added task PID %lu to sleep queue, wakeup at tick %lu", 
                task->pid, task->wakeup_time);
}

static void remove_from_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot remove NULL from sleep queue");
    KERNEL_ASSERT(g_sleep_queue.count > 0, "Sleep queue count underflow");

    if (task->wait_prev) {
        task->wait_prev->wait_next = task->wait_next;
    } else {
        g_sleep_queue.head = task->wait_next;
    }

    if (task->wait_next) {
        task->wait_next->wait_prev = task->wait_prev;
    }

    task->wait_next = NULL;
    task->wait_prev = NULL;
    g_sleep_queue.count--;
    
    SCHED_DEBUG("Removed task PID %lu from sleep queue", task->pid);
}

void scheduler_sleep_check_wakeups(void) {
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    
    if (!g_sleep_queue.head) { 
        spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags); 
        return; 
    }

    uint32_t current_ticks = scheduler_core_get_ticks();
    tcb_t *task = g_sleep_queue.head;
    bool task_woken = false;

    while (task && task->wakeup_time <= current_ticks) {
        tcb_t *task_to_wake = task;
        task = task->wait_next;
        
        remove_from_sleep_queue_locked(task_to_wake);
        task_to_wake->state = TASK_READY;
        
        // Release sleep lock before enqueueing
        spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);

        SCHED_DEBUG("Waking up task PID %lu (wakeup time %lu, current %lu)", 
                    task_to_wake->pid, task_to_wake->wakeup_time, current_ticks);
        
        if (!scheduler_queues_enqueue_ready_task(task_to_wake)) {
            SCHED_ERROR("Failed to enqueue woken task PID %lu", task_to_wake->pid);
        } else {
            task_woken = true;
        }

        // Re-acquire sleep lock for next iteration
        sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    }
    
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    
    if (task_woken) { 
        scheduler_core_set_need_reschedule();
    }
}

void scheduler_sleep_task(uint32_t ms) {
    if (ms == 0) { 
        scheduler_core_yield(); 
        return; 
    }
    
    uint32_t ticks_to_wait = MS_TO_TICKS(ms);
    if (ticks_to_wait == 0 && ms > 0) {
        ticks_to_wait = 1;
    }
    
    uint32_t current_ticks = scheduler_core_get_ticks();
    uint32_t wakeup_target;
    
    if (ticks_to_wait > (UINT32_MAX - current_ticks)) { 
        wakeup_target = UINT32_MAX; 
        SCHED_WARN("Sleep duration %lu ms results in tick overflow.", ms); 
    } else { 
        wakeup_target = current_ticks + ticks_to_wait; 
    }

    asm volatile("cli");
    tcb_t *current = scheduler_core_get_current_task();
    KERNEL_ASSERT(current && current->pid != IDLE_TASK_PID && 
                  (current->state == TASK_RUNNING || current->state == TASK_READY), 
                  "Invalid task state for sleep_ms");

    current->wakeup_time = wakeup_target;
    current->state = TASK_SLEEPING;
    current->in_run_queue = false;
    
    SCHED_DEBUG("Task PID %lu sleeping for %lu ms until tick %lu", 
                current->pid, ms, current->wakeup_time);

    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    add_to_sleep_queue_locked(current);
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    
    // Trigger reschedule to switch to another task
    scheduler_core_yield();
}

//============================================================================
// Sleep Queue Statistics & Debug
//============================================================================

uint32_t scheduler_sleep_get_sleeping_count(void) {
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    uint32_t count = g_sleep_queue.count;
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    return count;
}

void scheduler_sleep_debug_print_queue(void) {
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    
    serial_printf("[Sleep Queue] %lu sleeping tasks:\n", g_sleep_queue.count);
    
    tcb_t *task = g_sleep_queue.head;
    int index = 0;
    while (task && index < 10) { // Limit to first 10 for readability
        serial_printf("  [%d] PID %lu, wakeup at tick %lu\n", 
                      index++, task->pid, task->wakeup_time);
        task = task->wait_next;
    }
    
    if (task) {
        serial_printf("  ... and %lu more tasks\n", g_sleep_queue.count - index);
    }
    
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
}