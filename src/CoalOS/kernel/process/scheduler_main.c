/**
 * @file scheduler_main.c
 * @brief Main Scheduler Coordination and Public API
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Serves as the main coordination point for the modular scheduler
 * components. Provides the public scheduler API and delegates to specialized
 * modules. Follows the Facade pattern to provide a unified interface.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/process/scheduler.h>
#include <kernel/process/scheduler_core.h>
#include <kernel/process/scheduler_queues.h>
#include <kernel/process/scheduler_context.h>
#include <kernel/process/scheduler_sleep.h>
#include <kernel/process/scheduler_cleanup.h>
#include <kernel/process/scheduler_optimization.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Public API Implementation - Facade Pattern
//============================================================================

void scheduler_init(void) {
    terminal_printf("Initializing modular scheduler...\n");
    
    // Initialize all scheduler modules in dependency order
    scheduler_queues_init();
    scheduler_sleep_init();
    scheduler_context_init_idle_task();
    
#ifdef USE_SCHEDULER_OPTIMIZATION
    // Initialize scheduler optimizations
    scheduler_opt_init();
#endif
    
    // Core scheduler initialization
    scheduler_core_set_ready(false);
    
    terminal_printf("Modular scheduler initialized\n");
}

void scheduler_start(void) {
    terminal_printf("Starting modular scheduler...\n");
    scheduler_core_set_ready(true);
    
    // Try to select a user task to start with
    terminal_printf("  [Scheduler Start] Selecting initial task...\n");
    tcb_t *first_task = scheduler_select_next_task();
    
    if (!first_task) {
        // No user tasks available, enter idle mode directly
        terminal_printf("  [Scheduler Start] No tasks available, entering idle mode.\n");
        scheduler_context_enter_idle_mode();
        // This should not return
        KERNEL_PANIC_HALT("scheduler_start: Idle mode returned!");
    }
    
    // We have a user task to start
    KERNEL_ASSERT(first_task != NULL, "scheduler_start: scheduler_select_next_task returned NULL!");
    
    // Set up the first task
    volatile tcb_t *current_task_volatile = scheduler_core_get_current_task_volatile();
    // Update current task (this is a bit tricky due to the module separation)
    // For now, we'll use the core module's internal state
    
    terminal_printf("  [Scheduler Start] First task selected: PID %lu\n",
                     (unsigned long)first_task->pid);
    
    // Perform the initial context switch to the first task
    scheduler_context_switch(NULL, first_task);
    
    // Should not reach here if context switch is successful
    KERNEL_PANIC_HALT("scheduler_start: Initial task switch failed!");
}

int scheduler_add_task(pcb_t *pcb) {
    return scheduler_core_add_task(pcb);
}

void yield(void) {
    scheduler_core_yield();
}

void sleep_ms(uint32_t ms) {
    scheduler_sleep_task(ms);
}

void remove_current_task_with_code(uint32_t code) {
    scheduler_core_remove_current_task(code);
}

volatile tcb_t* get_current_task_volatile(void) {
    return scheduler_core_get_current_task_volatile();
}

tcb_t* get_current_task(void) {
    return scheduler_core_get_current_task();
}

void scheduler_tick(void) {
    scheduler_core_tick();
}

uint32_t scheduler_get_ticks(void) {
    return scheduler_core_get_ticks();
}

void scheduler_unblock_task(tcb_t *task) {
    scheduler_core_unblock_task(task);
}

bool scheduler_is_ready(void) {
    return scheduler_core_is_ready();
}

//============================================================================
// Priority Inheritance API - Delegation to Core
//============================================================================

uint8_t scheduler_get_effective_priority(tcb_t *task) {
    return scheduler_core_get_effective_priority(task);
}

void scheduler_inherit_priority(tcb_t *holder, tcb_t *waiter) {
    scheduler_core_inherit_priority(holder, waiter);
}

void scheduler_restore_priority(tcb_t *task) {
    scheduler_core_restore_priority(task);
}

void scheduler_add_blocked_task(tcb_t *holder, tcb_t *waiter) {
    scheduler_core_add_blocked_task(holder, waiter);
}

void scheduler_remove_blocked_task(tcb_t *holder, tcb_t *waiter) {
    scheduler_core_remove_blocked_task(holder, waiter);
}

//============================================================================
// Extended API for Module Access
//============================================================================

void scheduler_print_queue_stats(void) {
    scheduler_queues_debug_print_stats();
}

void scheduler_print_sleep_stats(void) {
    scheduler_sleep_debug_print_queue();
}

void scheduler_print_cleanup_stats(void) {
    scheduler_cleanup_print_stats();
}

//============================================================================
// Kernel Task Creation API
//============================================================================

int scheduler_create_kernel_task(void (*entry_point)(void), uint8_t priority, const char *name) {
    // This was in the original scheduler - we'll keep it here for compatibility
    // but it could be moved to a separate factory module
    
    if (!entry_point || priority >= 4) { // SCHED_PRIORITY_LEVELS = 4
        serial_printf("[Scheduler ERROR] Invalid parameters: entry_point=%p, priority=%u\n", 
                     entry_point, priority);
        return -1;
    }
    
    // Allocate TCB
    tcb_t *tcb = kmalloc(sizeof(tcb_t));
    if (!tcb) {
        serial_printf("[Scheduler ERROR] Failed to allocate TCB for kernel task '%s'\n", 
                     name ? name : "unnamed");
        return -1;
    }
    
    // Allocate PCB (minimal for kernel task)
    pcb_t *pcb = kmalloc(sizeof(pcb_t));
    if (!pcb) {
        kfree(tcb);
        serial_printf("[Scheduler ERROR] Failed to allocate PCB for kernel task '%s'\n", 
                     name ? name : "unnamed");
        return -1;
    }
    
    // Allocate kernel stack for the task
    void *kernel_stack = kmalloc(PROCESS_KSTACK_SIZE);
    if (!kernel_stack) {
        kfree(pcb);
        kfree(tcb);
        serial_printf("[Scheduler ERROR] Failed to allocate kernel stack for task '%s'\n", 
                     name ? name : "unnamed");
        return -1;
    }
    
    // Get next PID
    static uint32_t next_pid = 1;
    uint32_t task_pid = next_pid++;
    
    // Initialize PCB (minimal for kernel task)
    memset(pcb, 0, sizeof(pcb_t));
    pcb->pid = task_pid;
    pcb->is_kernel_task = true;
    
    // For kernel tasks, use the current kernel page directory
    extern uint32_t g_kernel_page_directory_phys;
    pcb->page_directory_phys = (uint32_t*)g_kernel_page_directory_phys;
    pcb->entry_point = (uint32_t)entry_point;
    pcb->kernel_stack_vaddr_top = (void*)((uintptr_t)kernel_stack + PROCESS_KSTACK_SIZE);
    
    // Set up initial context
    uint32_t *stack_ptr = (uint32_t*)pcb->kernel_stack_vaddr_top;
    --stack_ptr; *stack_ptr = (uint32_t)entry_point;
    
    pcb->user_stack_top = pcb->kernel_stack_vaddr_top;
    pcb->kernel_esp_for_switch = (uint32_t)stack_ptr;
    
    // Initialize TCB
    memset(tcb, 0, sizeof(tcb_t));
    tcb->process = pcb;
    tcb->pid = task_pid;
    tcb->state = TASK_READY;
    tcb->in_run_queue = false;
    tcb->has_run = false;
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->effective_priority = priority;
    tcb->context = stack_ptr;
    
    serial_printf("[Scheduler DEBUG] Created kernel task '%s': PID=%lu, priority=%u\n",
                name ? name : "unnamed", (unsigned long)task_pid, priority);
    
    // Add to scheduler using the core module
    scheduler_queues_add_to_all_tasks(tcb);
    
    if (!scheduler_queues_enqueue_ready_task(tcb)) {
        // Cleanup on failure
        kfree(kernel_stack);
        kfree(pcb);
        kfree(tcb);
        serial_printf("[Scheduler ERROR] Failed to enqueue kernel task '%s'\n", 
                     name ? name : "unnamed");
        return -1;
    }
    
    serial_printf("[Scheduler INFO] Added kernel task PID %lu\n", (unsigned long)task_pid);
    return 0;
}

//============================================================================
// Debug Functions
//============================================================================

void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches) {
    // Simplified implementation
    if (out_task_count) {
        *out_task_count = 0;
        for (int i = 0; i < 4; i++) {
            *out_task_count += scheduler_queues_get_count(i);
        }
        *out_task_count += scheduler_sleep_get_sleeping_count();
    }
    
    if (out_switches) {
        *out_switches = 0; // Not tracked in this implementation
    }
}

void check_idle_task_stack_integrity(const char *checkpoint) {
    scheduler_context_check_idle_integrity(checkpoint);
}