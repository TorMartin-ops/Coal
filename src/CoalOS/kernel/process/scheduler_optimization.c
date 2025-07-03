/**
 * @file scheduler_optimization.c
 * @brief Scheduler Optimization Implementation for Coal OS
 * @version 1.0
 * @author Performance optimization
 */

#include <kernel/process/scheduler_optimization.h>
#include <kernel/process/scheduler_queues.h>
#include <kernel/process/scheduler_core.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/string.h>

//============================================================================
// Configuration
//============================================================================
#define MAX_TASKS 256  // Maximum number of tasks supported

//============================================================================
// Module Static Data
//============================================================================

static priority_bitmap_t g_active_priorities;
static scheduler_load_t g_load_stats;
static task_stats_t *g_task_stats[MAX_TASKS];  // Assume MAX_TASKS is defined

// Per-priority queue counters for fast empty detection
static uint32_t g_queue_counts[SCHED_PRIORITY_LEVELS];

//============================================================================
// Logging
//============================================================================

#define OPT_DEBUG(fmt, ...) serial_printf("[Sched OPT  ] " fmt "\n", ##__VA_ARGS__)
#define OPT_INFO(fmt, ...)  serial_printf("[Sched OPT  ] " fmt "\n", ##__VA_ARGS__)

//============================================================================
// Initialization
//============================================================================

void scheduler_opt_init(void) {
    // Clear bitmap
    memset(&g_active_priorities, 0, sizeof(g_active_priorities));
    
    // Initialize load statistics
    memset(&g_load_stats, 0, sizeof(g_load_stats));
    
    // Clear queue counters
    memset(g_queue_counts, 0, sizeof(g_queue_counts));
    
    // Clear task stats
    memset(g_task_stats, 0, sizeof(g_task_stats));
    
    OPT_INFO("Scheduler optimizations initialized");
}

//============================================================================
// Priority Bitmap Management
//============================================================================

void scheduler_opt_mark_priority_active(uint8_t priority) {
    if (priority < SCHED_PRIORITY_LEVELS) {
        bitmap_set_priority(&g_active_priorities, priority);
        g_queue_counts[priority]++;
        OPT_DEBUG("Priority %u marked active (count: %u)", priority, g_queue_counts[priority]);
    }
}

void scheduler_opt_mark_priority_empty(uint8_t priority) {
    if (priority < SCHED_PRIORITY_LEVELS) {
        if (g_queue_counts[priority] > 0) {
            g_queue_counts[priority]--;
        }
        
        if (g_queue_counts[priority] == 0) {
            bitmap_clear_priority(&g_active_priorities, priority);
            OPT_DEBUG("Priority %u marked empty", priority);
        }
    }
}

//============================================================================
// Optimized Task Selection
//============================================================================

tcb_t* scheduler_opt_select_next_task(void) {
    // Find highest priority with tasks using bitmap
    int highest_prio = bitmap_find_first_set(&g_active_priorities);
    
    if (highest_prio < 0 || highest_prio >= SCHED_PRIORITY_LEVELS) {
        return NULL;  // No tasks available
    }
    
    // Get task from that priority queue
    tcb_t *task = scheduler_queues_dequeue_ready_task((uint8_t)highest_prio);
    
    if (task) {
        // Update bitmap if queue is now empty
        g_queue_counts[highest_prio]--;
        if (g_queue_counts[highest_prio] == 0) {
            bitmap_clear_priority(&g_active_priorities, highest_prio);
        }
        
        // Update task statistics
        if (task->pid < MAX_TASKS && g_task_stats[task->pid]) {
            g_task_stats[task->pid]->wait_ticks = 0;  // Reset wait time
        }
        
        OPT_DEBUG("Selected task PID %lu from priority %d (O(1) selection)", 
                  task->pid, highest_prio);
    }
    
    return task;
}

//============================================================================
// Load Statistics
//============================================================================

void scheduler_opt_update_load_stats(void) {
    // Count tasks in each state
    uint32_t runnable = 0;
    uint32_t blocked = 0;
    uint32_t total = 0;
    
    // This would iterate through all tasks - simplified for now
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        runnable += g_queue_counts[i];
    }
    
    g_load_stats.runnable_tasks = runnable;
    g_load_stats.total_tasks = total;
    g_load_stats.blocked_tasks = blocked;
    
    // Update load history
    g_load_stats.load_history[g_load_stats.history_index] = runnable;
    g_load_stats.history_index = (g_load_stats.history_index + 1) % SCHED_LOAD_HISTORY_SIZE;
    
    // Calculate average
    uint32_t sum = 0;
    for (int i = 0; i < SCHED_LOAD_HISTORY_SIZE; i++) {
        sum += g_load_stats.load_history[i];
    }
    g_load_stats.average_load = sum / SCHED_LOAD_HISTORY_SIZE;
}

//============================================================================
// Dynamic Priority Boosting
//============================================================================

bool scheduler_opt_should_boost_priority(tcb_t *task) {
    if (!task || task->pid >= MAX_TASKS) return false;
    
    task_stats_t *stats = g_task_stats[task->pid];
    if (!stats) return false;
    
    // Check if task has been waiting too long
    if (stats->wait_ticks > SCHED_BOOST_THRESHOLD) {
        // Don't boost if already at highest priority
        if (task->effective_priority > 0) {
            return true;
        }
    }
    
    // Check if task is interactive (I/O bound)
    if (stats->is_interactive && task->effective_priority > 1) {
        return true;
    }
    
    return false;
}

void scheduler_opt_boost_priority(tcb_t *task) {
    if (!task || task->effective_priority == 0) return;
    
    uint8_t old_priority = task->effective_priority;
    task->effective_priority--;  // Lower number = higher priority
    
    if (task->pid < MAX_TASKS && g_task_stats[task->pid]) {
        g_task_stats[task->pid]->boost_count++;
    }
    
    OPT_INFO("Boosted task PID %lu priority %u -> %u", 
             task->pid, old_priority, task->effective_priority);
    
    // If task is in run queue, move it to new priority
    if (task->in_run_queue && task->state == TASK_READY) {
        scheduler_queues_move_task_priority(task, old_priority, task->effective_priority);
        
        // Update bitmap
        scheduler_opt_mark_priority_active(task->effective_priority);
        if (g_queue_counts[old_priority] == 0) {
            scheduler_opt_mark_priority_empty(old_priority);
        }
    }
}

void scheduler_opt_reset_boost(tcb_t *task) {
    if (!task) return;
    
    if (task->effective_priority != task->base_priority) {
        uint8_t old_priority = task->effective_priority;
        task->effective_priority = task->base_priority;
        
        OPT_DEBUG("Reset task PID %lu priority %u -> %u (base)", 
                  task->pid, old_priority, task->effective_priority);
        
        if (task->pid < MAX_TASKS && g_task_stats[task->pid]) {
            g_task_stats[task->pid]->boost_count = 0;
            g_task_stats[task->pid]->wait_ticks = 0;
        }
    }
}

//============================================================================
// Load Analysis
//============================================================================

uint32_t scheduler_opt_get_load_average(void) {
    return g_load_stats.average_load;
}

bool scheduler_opt_is_high_load(void) {
    // Consider high load if more than 75% of priority queues have tasks
    uint32_t active_queues = 0;
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        if (g_queue_counts[i] > 0) active_queues++;
    }
    
    return (active_queues * 100 / SCHED_PRIORITY_LEVELS) > 75;
}

void scheduler_opt_get_queue_stats(scheduler_load_t *stats) {
    if (stats) {
        memcpy(stats, &g_load_stats, sizeof(scheduler_load_t));
    }
}

//============================================================================
// Task Statistics Management
//============================================================================

void scheduler_opt_alloc_task_stats(uint32_t pid) {
    if (pid < MAX_TASKS && !g_task_stats[pid]) {
        g_task_stats[pid] = kmalloc(sizeof(task_stats_t));
        if (g_task_stats[pid]) {
            memset(g_task_stats[pid], 0, sizeof(task_stats_t));
        }
    }
}

void scheduler_opt_free_task_stats(uint32_t pid) {
    if (pid < MAX_TASKS && g_task_stats[pid]) {
        kfree(g_task_stats[pid]);
        g_task_stats[pid] = NULL;
    }
}

void scheduler_opt_update_wait_ticks(tcb_t *task) {
    if (task && task->pid < MAX_TASKS && g_task_stats[task->pid]) {
        g_task_stats[task->pid]->wait_ticks++;
    }
}

void scheduler_opt_mark_interactive(tcb_t *task) {
    if (task && task->pid < MAX_TASKS && g_task_stats[task->pid]) {
        g_task_stats[task->pid]->is_interactive = true;
    }
}