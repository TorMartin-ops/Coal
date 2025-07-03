/**
 * @file process_hierarchy.c
 * @brief Process hierarchy management for Linux compatibility
 * 
 * Handles parent-child relationships, process groups, sessions, and process reaping.
 * Separated to focus on process tree management concerns.
 */

#include <kernel/process/process.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/sync/spinlock.h>
#include <kernel/fs/vfs/fs_errno.h>

/**
 * @brief Establishes parent-child relationship between processes.
 */
void process_add_child(pcb_t *parent, pcb_t *child) {
    if (!parent || !child) return;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    // Set up parent-child relationship
    child->parent = parent;
    child->ppid = parent->pid;
    
    // Add child to parent's children list (insert at head)
    child->sibling = parent->children;
    parent->children = child;
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    
    serial_printf("[Process] Established parent-child relationship: PID %u -> PID %u\n",
                  parent->pid, child->pid);
}

/**
 * @brief Removes a child from parent's children list.
 */
void process_remove_child(pcb_t *parent, pcb_t *child) {
    if (!parent || !child) return;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    // Find and remove child from siblings list
    if (parent->children == child) {
        // Child is first in the list
        parent->children = child->sibling;
    } else {
        // Search for child in the list
        pcb_t *current = parent->children;
        while (current && current->sibling != child) {
            current = current->sibling;
        }
        if (current) {
            current->sibling = child->sibling;
        }
    }
    
    // Clear child's family pointers
    child->parent = NULL;
    child->ppid = 0;
    child->sibling = NULL;
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    
    serial_printf("[Process] Removed child PID %u from parent PID %u\n",
                  child->pid, parent->pid);
}

/**
 * @brief Finds a child process by PID.
 */
pcb_t *process_find_child(pcb_t *parent, uint32_t child_pid) {
    if (!parent) return NULL;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    pcb_t *current = parent->children;
    while (current) {
        if (current->pid == child_pid) {
            spinlock_release_irqrestore(&parent->children_lock, flags);
            return current;
        }
        current = current->sibling;
    }
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    return NULL;
}

/**
 * @brief Marks a process as exited and notifies parent.
 */
void process_exit_with_status(pcb_t *proc, uint32_t exit_status) {
    if (!proc) return;
    
    proc->exit_status = exit_status;
    proc->has_exited = true;
    proc->state = PROC_ZOMBIE;
    
    serial_printf("[Process] PID %u exited with status %u\n", proc->pid, exit_status);
    
    // If process has children, reparent them to init process (PID 1)
    if (proc->children) {
        // TODO: Find init process and reparent children
        // For now, just orphan them
        uintptr_t flags = spinlock_acquire_irqsave(&proc->children_lock);
        pcb_t *child = proc->children;
        while (child) {
            pcb_t *next_child = child->sibling;
            child->parent = NULL;
            child->ppid = 1; // Reparent to init
            child->sibling = NULL;
            child = next_child;
        }
        proc->children = NULL;
        spinlock_release_irqrestore(&proc->children_lock, flags);
        
        serial_printf("[Process] Reparented children of PID %u to init\n", proc->pid);
    }
    
    // TODO: Wake up parent if it's waiting in waitpid()
}

/**
 * @brief Reaps zombie children and cleans up their resources.
 */
int process_reap_child(pcb_t *parent, int child_pid, int *status) {
    if (!parent) return -ESRCH;
    
    uintptr_t flags = spinlock_acquire_irqsave(&parent->children_lock);
    
    pcb_t *child_to_reap = NULL;
    
    if (child_pid == -1) {
        // Wait for any child - find first zombie
        pcb_t *current = parent->children;
        while (current) {
            if (current->has_exited) {
                child_to_reap = current;
                break;
            }
            current = current->sibling;
        }
    } else {
        // Wait for specific child
        child_to_reap = process_find_child(parent, child_pid);
        if (child_to_reap && !child_to_reap->has_exited) {
            child_to_reap = NULL; // Child exists but hasn't exited
        }
    }
    
    if (!child_to_reap) {
        spinlock_release_irqrestore(&parent->children_lock, flags);
        return child_pid == -1 ? -ECHILD : -ECHILD; // No zombie children
    }
    
    // Copy exit status
    if (status) {
        *status = child_to_reap->exit_status;
    }
    
    uint32_t reaped_pid = child_to_reap->pid;
    
    // Remove from children list
    if (parent->children == child_to_reap) {
        parent->children = child_to_reap->sibling;
    } else {
        pcb_t *current = parent->children;
        while (current && current->sibling != child_to_reap) {
            current = current->sibling;
        }
        if (current) {
            current->sibling = child_to_reap->sibling;
        }
    }
    
    spinlock_release_irqrestore(&parent->children_lock, flags);
    
    // Clean up child resources
    serial_printf("[Process] Reaping zombie child PID %u (exit status %u)\n",
                  reaped_pid, child_to_reap->exit_status);
    
    // TODO: Call destroy_process(child_to_reap) to free all resources
    // destroy_process(child_to_reap);
    
    return reaped_pid;
}