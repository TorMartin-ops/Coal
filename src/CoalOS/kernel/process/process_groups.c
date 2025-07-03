/**
 * @file process_groups.c
 * @brief Process groups and sessions management for POSIX compatibility
 * 
 * Handles process groups, sessions, and terminal control for Linux compatibility.
 * Separated to focus on process group and session concerns.
 */

#include <kernel/process/process.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>

/**
 * @brief Initializes process group and session fields in a new PCB.
 */
void process_init_pgrp_session(pcb_t *proc, pcb_t *parent) {
    if (!proc) return;
    
    proc->pid_namespace = 0; // Default namespace
    
    if (!parent) {
        // Init process - create new session and process group
        proc->sid = proc->pid;
        proc->pgid = proc->pid;
        proc->session_leader = proc;
        proc->pgrp_leader = proc;
        proc->is_session_leader = true;
        proc->is_pgrp_leader = true;
        proc->controlling_terminal = NULL;
        proc->has_controlling_tty = false;
        proc->tty_pgrp = proc->pid;
    } else {
        // Inherit from parent
        proc->sid = parent->sid;
        proc->pgid = parent->pgid;
        proc->session_leader = parent->session_leader;
        proc->pgrp_leader = parent->pgrp_leader;
        proc->is_session_leader = false;
        proc->is_pgrp_leader = false;
        proc->controlling_terminal = parent->controlling_terminal;
        proc->has_controlling_tty = parent->has_controlling_tty;
        proc->tty_pgrp = parent->tty_pgrp;
        
        // Add to parent's process group
        if (parent->pgrp_leader) {
            process_join_pgrp(proc, parent->pgrp_leader);
        }
    }
    
    proc->pgrp_next = NULL;
    proc->pgrp_prev = NULL;
    
    serial_printf("[Process] Initialized PID %u: SID=%u, PGID=%u, Session Leader=%s, PGrp Leader=%s\n",
                  proc->pid, proc->sid, proc->pgid,
                  proc->is_session_leader ? "Yes" : "No",
                  proc->is_pgrp_leader ? "Yes" : "No");
}

int process_setsid(pcb_t *proc) {
    if (!proc) return -ESRCH;
    
    // Cannot create session if already a process group leader
    if (proc->is_pgrp_leader) {
        return -EPERM;
    }
    
    // Leave current process group
    process_leave_pgrp(proc);
    
    // Create new session and process group
    proc->sid = proc->pid;
    proc->pgid = proc->pid;
    proc->session_leader = proc;
    proc->pgrp_leader = proc;
    proc->is_session_leader = true;
    proc->is_pgrp_leader = true;
    
    // Lose controlling terminal
    proc->controlling_terminal = NULL;
    proc->has_controlling_tty = false;
    proc->tty_pgrp = proc->pid;
    
    serial_printf("[Process] PID %u created new session (SID=%u)\n", proc->pid, proc->sid);
    return proc->sid;
}

uint32_t process_getsid(pcb_t *proc) {
    if (!proc) return 0;
    return proc->sid;
}

int process_setpgid(pcb_t *proc, uint32_t pgid) {
    if (!proc) return -ESRCH;
    
    // If pgid is 0, use process PID
    if (pgid == 0) {
        pgid = proc->pid;
    }
    
    // Cannot change pgid if session leader
    if (proc->is_session_leader) {
        return -EPERM;
    }
    
    // Find the target process group leader
    pcb_t *new_pgrp_leader = NULL;
    if (pgid == proc->pid) {
        // Creating new process group with self as leader
        new_pgrp_leader = proc;
    } else {
        // TODO: Find process group leader by PGID
        // For now, simplified implementation
        new_pgrp_leader = proc->pgrp_leader;
    }
    
    if (!new_pgrp_leader) {
        return -ESRCH;
    }
    
    // Leave current process group
    process_leave_pgrp(proc);
    
    // Join new process group
    int result = process_join_pgrp(proc, new_pgrp_leader);
    if (result == 0) {
        proc->pgid = pgid;
        if (pgid == proc->pid) {
            proc->is_pgrp_leader = true;
            proc->pgrp_leader = proc;
        }
        
        serial_printf("[Process] PID %u joined process group %u\n", proc->pid, pgid);
    }
    
    return result;
}

uint32_t process_getpgid(pcb_t *proc) {
    if (!proc) return 0;
    return proc->pgid;
}

int process_join_pgrp(pcb_t *proc, pcb_t *pgrp_leader) {
    if (!proc || !pgrp_leader) return -EINVAL;
    
    // Ensure both processes are in same session
    if (proc->sid != pgrp_leader->sid) {
        return -EPERM;
    }
    
    // Add to process group linked list
    proc->pgrp_leader = pgrp_leader;
    proc->pgrp_next = pgrp_leader->pgrp_next;
    proc->pgrp_prev = pgrp_leader;
    
    if (pgrp_leader->pgrp_next) {
        pgrp_leader->pgrp_next->pgrp_prev = proc;
    }
    pgrp_leader->pgrp_next = proc;
    
    proc->pgid = pgrp_leader->pgid;
    proc->is_pgrp_leader = (proc == pgrp_leader);
    
    serial_printf("[Process] PID %u joined process group led by PID %u\n", 
                  proc->pid, pgrp_leader->pid);
    
    return 0;
}

void process_leave_pgrp(pcb_t *proc) {
    if (!proc) return;
    
    // Remove from process group linked list
    if (proc->pgrp_prev) {
        proc->pgrp_prev->pgrp_next = proc->pgrp_next;
    }
    if (proc->pgrp_next) {
        proc->pgrp_next->pgrp_prev = proc->pgrp_prev;
    }
    
    // If this was the process group leader, transfer leadership
    if (proc->is_pgrp_leader && proc->pgrp_next) {
        pcb_t *new_leader = proc->pgrp_next;
        new_leader->is_pgrp_leader = true;
        new_leader->pgrp_leader = new_leader;
        
        // Update all processes in the group
        pcb_t *current = new_leader;
        while (current) {
            current->pgrp_leader = new_leader;
            current->pgid = new_leader->pid;
            current = current->pgrp_next;
        }
        
        serial_printf("[Process] PID %u transferred process group leadership to PID %u\n",
                      proc->pid, new_leader->pid);
    }
    
    proc->pgrp_next = NULL;
    proc->pgrp_prev = NULL;
    proc->pgrp_leader = proc; // Self-reference
    proc->is_pgrp_leader = true;
    proc->pgid = proc->pid;
    
    serial_printf("[Process] PID %u left process group\n", proc->pid);
}

int process_tcsetpgrp(pcb_t *proc, uint32_t pgid) {
    if (!proc) return -ESRCH;
    
    // Only session leader can change foreground process group
    if (!proc->is_session_leader) {
        return -EPERM;
    }
    
    // Must have controlling terminal
    if (!proc->has_controlling_tty) {
        return -ENOTTY;
    }
    
    // TODO: Validate that pgid exists in this session
    
    proc->tty_pgrp = pgid;
    
    serial_printf("[Process] Session leader PID %u set foreground group to %u\n", 
                  proc->pid, pgid);
    
    return 0;
}

int process_tcgetpgrp(pcb_t *proc) {
    if (!proc) return -ESRCH;
    
    if (!proc->has_controlling_tty) {
        return -ENOTTY;
    }
    
    return proc->tty_pgrp;
}