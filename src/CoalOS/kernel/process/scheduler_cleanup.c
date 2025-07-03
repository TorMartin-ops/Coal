/**
 * @file scheduler_cleanup.c
 * @brief Zombie Process Cleanup and Resource Management
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles cleanup of terminated processes, resource deallocation,
 * and zombie process reaping. Focuses purely on cleanup operations
 * without affecting core scheduling logic.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/process/scheduler_cleanup.h>
#include <kernel/process/scheduler_queues.h>
#include <kernel/process/scheduler_context.h>
#include <kernel/process/process_manager.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/assert.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Cleanup Configuration
//============================================================================
#define IDLE_TASK_PID 0

// Logging Macros
#define SCHED_INFO(fmt, ...)  serial_printf("[Cleanup INFO ] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) serial_printf("[Cleanup DEBUG] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) serial_printf("[Cleanup ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_WARN(fmt, ...)  serial_printf("[Cleanup WARN ] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_TRACE(fmt, ...) ((void)0)

//============================================================================
// Zombie Cleanup Implementation
//============================================================================

void scheduler_cleanup_zombies(void) {
    SCHED_TRACE("Checking for ZOMBIE tasks...");
    
    tcb_t *zombie_to_reap = scheduler_queues_remove_zombie_task();
    
    if (zombie_to_reap) {
        SCHED_INFO("Cleanup: Reaping ZOMBIE task PID %lu (Exit Code: %lu).", 
                   zombie_to_reap->pid, zombie_to_reap->exit_code);
        
        // Check idle task stack before destroying process
        scheduler_context_check_idle_integrity("Before destroy_process");
        
        if (zombie_to_reap->process) {
            serial_printf("[destroy_process] Enter for PID %lu\n", zombie_to_reap->pid);
            destroy_process(zombie_to_reap->process);
            serial_printf("[destroy_process] Exit for PID %lu\n", zombie_to_reap->pid);
        } else {
            SCHED_WARN("Zombie task PID %lu has NULL process pointer!", zombie_to_reap->pid);
        }
        
        // Check idle task stack after destroying process
        scheduler_context_check_idle_integrity("After destroy_process");
        
        // Free the TCB
        kfree(zombie_to_reap);
        
        // Check idle task stack after freeing TCB
        scheduler_context_check_idle_integrity("After kfree(tcb)");
        
        SCHED_DEBUG("Successfully reaped zombie task PID %lu", zombie_to_reap->pid);
    }
}

void scheduler_cleanup_all_zombies(void) {
    int cleanup_count = 0;
    const int max_cleanup_per_call = 10; // Prevent infinite loops
    
    SCHED_DEBUG("Starting comprehensive zombie cleanup");
    
    while (cleanup_count < max_cleanup_per_call) {
        tcb_t *zombie_to_reap = scheduler_queues_remove_zombie_task();
        
        if (!zombie_to_reap) {
            break; // No more zombies
        }
        
        SCHED_INFO("Bulk cleanup: Reaping ZOMBIE task PID %lu (Exit Code: %lu).", 
                   zombie_to_reap->pid, zombie_to_reap->exit_code);
        
        if (zombie_to_reap->process) {
            destroy_process(zombie_to_reap->process);
        }
        
        kfree(zombie_to_reap);
        cleanup_count++;
    }
    
    if (cleanup_count > 0) {
        SCHED_INFO("Bulk cleanup completed: reaped %d zombie tasks", cleanup_count);
    } else {
        SCHED_DEBUG("No zombie tasks found during bulk cleanup");
    }
    
    if (cleanup_count == max_cleanup_per_call) {
        SCHED_WARN("Hit cleanup limit - more zombies may remain");
    }
}

bool scheduler_cleanup_force_cleanup_task(uint32_t pid) {
    if (pid == IDLE_TASK_PID) {
        SCHED_ERROR("Cannot force cleanup idle task PID %lu", (unsigned long)pid);
        return false;
    }
    
    SCHED_DEBUG("Attempting forced cleanup of task PID %lu", pid);
    
    // This is a simplified implementation
    // In a full implementation, we would search through all task lists
    // and forcibly clean up the specified task
    
    tcb_t *zombie = scheduler_queues_remove_zombie_task();
    while (zombie) {
        if (zombie->pid == pid) {
            SCHED_INFO("Force cleanup: Found and reaping task PID %lu", pid);
            
            if (zombie->process) {
                destroy_process(zombie->process);
            }
            kfree(zombie);
            
            SCHED_DEBUG("Successfully force-cleaned task PID %lu", pid);
            return true;
        }
        
        // Put it back and continue searching
        // Note: This is simplified - in reality we'd need a proper search
        kfree(zombie);
        zombie = scheduler_queues_remove_zombie_task();
    }
    
    SCHED_WARN("Force cleanup: Task PID %lu not found in zombie list", pid);
    return false;
}

//============================================================================
// Cleanup Statistics & Monitoring
//============================================================================

typedef struct {
    uint32_t total_reaped;
    uint32_t last_reap_tick;
    uint32_t reap_failures;
} cleanup_stats_t;

static cleanup_stats_t g_cleanup_stats = {0};

void scheduler_cleanup_get_stats(uint32_t *total_reaped, uint32_t *last_reap_tick, uint32_t *failures) {
    if (total_reaped) *total_reaped = g_cleanup_stats.total_reaped;
    if (last_reap_tick) *last_reap_tick = g_cleanup_stats.last_reap_tick;
    if (failures) *failures = g_cleanup_stats.reap_failures;
}

void scheduler_cleanup_increment_stats(bool success) {
    if (success) {
        g_cleanup_stats.total_reaped++;
        // Would set last_reap_tick to current tick if we had access to scheduler_core_get_ticks()
    } else {
        g_cleanup_stats.reap_failures++;
    }
}

void scheduler_cleanup_print_stats(void) {
    serial_printf("[Cleanup Stats] Total reaped: %lu, Failures: %lu\n",
                  g_cleanup_stats.total_reaped, g_cleanup_stats.reap_failures);
}

//============================================================================
// Resource Validation
//============================================================================

bool scheduler_cleanup_validate_task_resources(tcb_t *task) {
    if (!task) {
        SCHED_ERROR("Cannot validate NULL task");
        return false;
    }
    
    // Basic validation checks
    if (task->pid == 0 && task->pid != IDLE_TASK_PID) {
        SCHED_ERROR("Task has invalid PID 0 (not idle task)");
        return false;
    }
    
    if (task->state == TASK_ZOMBIE && task->process == NULL) {
        SCHED_WARN("Zombie task PID %lu has NULL process - may cause cleanup issues", task->pid);
        return false;
    }
    
    if (task->in_run_queue && task->state == TASK_ZOMBIE) {
        SCHED_ERROR("Zombie task PID %lu still marked as in_run_queue", task->pid);
        return false;
    }
    
    SCHED_TRACE("Task PID %lu passed resource validation", task->pid);
    return true;
}