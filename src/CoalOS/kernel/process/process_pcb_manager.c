/**
 * @file process_pcb_manager.c
 * @brief Process Control Block (PCB) allocation and basic management
 * 
 * Handles the allocation, initialization, and destruction of PCB structures.
 * Follows Single Responsibility Principle by focusing solely on PCB lifecycle.
 */

#include <kernel/process/process.h>
#include <kernel/process/process_manager.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/process/scheduler.h>
#include <kernel/sync/spinlock.h>

// Process ID counter - NEEDS LOCKING FOR SMP
static uint32_t next_pid = 1;
// TODO: Add spinlock_t g_pid_lock;

/**
 * @brief Allocates and initializes a basic PCB structure
 * @param name Process name (for debugging)
 * @return Pointer to the newly allocated PCB, or NULL on failure
 */
pcb_t* process_create(const char* name) {
    // Validate and provide safe default for name
    if (!name || name[0] == '\0') {
        name = "unnamed";
    }
    
    // Validate name length to prevent buffer overflows
    size_t name_len = strlen(name);
    if (name_len >= 256) { // Reasonable process name limit
        serial_printf("[Process] Process name too long: %zu chars\n", name_len);
        return NULL;
    }
    
    pcb_t* proc = (pcb_t*)kmalloc(sizeof(pcb_t));
    if (!proc) {
        serial_printf("[Process] Failed to allocate PCB for '%s'\n", name);
        return NULL;
    }
    
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;
    proc->state = PROC_INITIALIZING;
    
    // Initialize spinlock
    spinlock_init(&proc->fd_table_lock);
    
    // Initialize process hierarchy and process groups/sessions
    process_init_hierarchy(proc);
    process_init_pgrp_session(proc, NULL); // NULL parent = new session leader
    
    serial_printf("[Process] Created PCB for '%s' with PID %u\n", name, proc->pid);
    return proc;
}

/**
 * @brief Gets the PCB of the currently running process.
 * @note Relies on the scheduler providing the current task/thread control block.
 * @return Pointer to the current PCB, or NULL if no process context is active.
 */
pcb_t* get_current_process(void)
{
     // Assumes scheduler maintains the current task control block (TCB)
     // Needs adaptation based on your actual scheduler implementation.
     tcb_t* current_tcb = get_current_task(); // Assuming get_current_task() exists and returns tcb_t*
     if (current_tcb && current_tcb->process) { // Assuming tcb_t has a 'process' field pointing to pcb_t
         return current_tcb->process;
     }
     // Could be running early boot code or a kernel-only thread without a full PCB
     return NULL;
}

/**
 * @brief Initializes process hierarchy fields in a new PCB.
 */
void process_init_hierarchy(pcb_t *proc) {
    if (!proc) return;
    
    proc->ppid = 0;
    proc->parent = NULL;
    proc->children = NULL;
    proc->sibling = NULL;
    proc->exit_status = 0;
    proc->has_exited = false;
    spinlock_init(&proc->children_lock);
}

//============================================================================
// New Standardized Process Management API Implementation
//============================================================================

error_t process_create_safe(const char* name, pcb_t** proc_out) {
    // Input validation
    if (!name || !proc_out) {
        return E_INVAL;
    }
    
    // Basic name validation
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= 256) { // Reasonable name length limit
        return E_INVAL;
    }

    // Check for PID overflow (simplified check)
    if (next_pid == 0 || next_pid > 65535) { // Reasonable PID limit
        serial_printf("[Process] PID counter overflow (current: %u)\n", next_pid);
        return E_OVERFLOW;
    }

    // Allocate PCB using kmalloc
    pcb_t* proc = (pcb_t*)kmalloc(sizeof(pcb_t));
    if (!proc) {
        serial_printf("[Process] Failed to allocate PCB for '%s': insufficient memory\n", name);
        return E_NOMEM;
    }
    
    // Initialize PCB structure
    memset(proc, 0, sizeof(pcb_t));
    proc->pid = next_pid++;
    proc->state = PROC_INITIALIZING;
    
    // Initialize synchronization primitives
    spinlock_init(&proc->fd_table_lock);
    
    // Initialize process hierarchy and process groups/sessions
    process_init_hierarchy(proc);
    process_init_pgrp_session(proc, NULL); // NULL parent = new session leader
    
    // Success
    *proc_out = proc;
    serial_printf("[Process] Created PCB for '%s' with PID %u\n", name, proc->pid);
    return E_SUCCESS;
}

error_t get_current_process_safe(pcb_t** proc_out) {
    // Input validation
    if (!proc_out) {
        return E_INVAL;
    }

    // Get current task from scheduler
    tcb_t* current_tcb = get_current_task();
    if (!current_tcb) {
        // No current task context - might be early boot or kernel thread
        return E_NOTFOUND;
    }

    // Validate TCB structure
    if (!current_tcb->process) {
        // TCB exists but no associated process - kernel thread or corrupted state
        return E_NOTFOUND;
    }

    // Additional sanity check on the process pointer
    pcb_t* proc = current_tcb->process;
    if (proc->pid == 0 || proc->state == PROC_ZOMBIE) {
        // Process is in invalid state
        return E_FAULT;
    }

    // Success
    *proc_out = proc;
    return E_SUCCESS;
}