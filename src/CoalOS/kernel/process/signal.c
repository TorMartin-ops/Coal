/**
 * @file signal.c
 * @brief Comprehensive Signal Handling System for CoalOS
 * @version 1.0
 * @author Claude Code
 * 
 * Implements POSIX-compatible signal handling with:
 * - Signal registration and delivery
 * - Signal masking and queuing
 * - Integration with scheduler and interrupts
 * - User-space signal handler execution
 */

#include <kernel/process/signal.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/uaccess.h>
#include <kernel/lib/string.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/core/error.h>
#include <libc/stddef.h>

// Forward declaration for process lookup
extern pcb_t *process_get_by_pid(uint32_t pid);

// Error constants compatibility
#define EINVAL E_INVAL
#define ESRCH E_NOTFOUND
#define EPERM E_PERM
#define ECHILD E_NOTFOUND
#define EFAULT E_FAULT
#define ENOMEM E_NOMEM

//============================================================================
// Signal Default Actions Table
//============================================================================

// Signal default actions: 0=ignore, 1=terminate, 2=stop, 3=continue, 4=coredump
static const int signal_default_actions[SIGNAL_MAX] = {
    0,  // Signal 0 (not used)
    1,  // SIGHUP    - terminate
    1,  // SIGINT    - terminate  
    4,  // SIGQUIT   - coredump
    4,  // SIGILL    - coredump
    2,  // SIGTRAP   - stop
    4,  // SIGABRT   - coredump
    4,  // SIGBUS    - coredump
    4,  // SIGFPE    - coredump
    1,  // SIGKILL   - terminate (uncatchable)
    1,  // SIGUSR1   - terminate
    4,  // SIGSEGV   - coredump
    1,  // SIGUSR2   - terminate
    1,  // SIGPIPE   - terminate
    1,  // SIGALRM   - terminate
    1,  // SIGTERM   - terminate
    1,  // SIGSTKFLT - terminate
    0,  // SIGCHLD   - ignore
    3,  // SIGCONT   - continue
    2,  // SIGSTOP   - stop (uncatchable)
    2,  // SIGTSTP   - stop
    2,  // SIGTTIN   - stop
    2,  // SIGTTOU   - stop
    0,  // SIGURG    - ignore
    1,  // SIGXCPU   - terminate
    1,  // SIGXFSZ   - terminate
    1,  // SIGVTALRM - terminate
    1,  // SIGPROF   - terminate
    0,  // SIGWINCH  - ignore
    1,  // SIGIO     - terminate
    1,  // SIGPWR    - terminate
    4   // SIGSYS    - coredump
};

//============================================================================
// Signal Initialization
//============================================================================

void signal_init_process(pcb_t *proc) {
    if (!proc) return;
    
    // Initialize signal-related fields
    proc->signal_mask = 0;          // No signals blocked initially
    proc->pending_signals = 0;      // No pending signals
    proc->signal_altstack = NULL;   // No alternate stack
    proc->signal_flags = 0;         // No special flags
    proc->in_signal_handler = 0;    // Not in signal handler
    
    // Initialize all signal handlers to default
    for (int i = 0; i < 32; i++) {
        proc->signal_handlers[i] = SIG_DFL;
    }
    
    // Initialize signal lock
    spinlock_init(&proc->signal_lock);
    
    serial_printf("[Signal] Initialized signal handling for PID %u\n", proc->pid);
}

void signal_copy_handlers(pcb_t *parent, pcb_t *child) {
    if (!parent || !child) return;
    
    uintptr_t parent_flags = spinlock_acquire_irqsave(&parent->signal_lock);
    uintptr_t child_flags = spinlock_acquire_irqsave(&child->signal_lock);
    
    // Copy signal mask and handlers
    child->signal_mask = parent->signal_mask;
    child->pending_signals = 0;  // Child starts with no pending signals
    child->signal_flags = parent->signal_flags;
    child->signal_altstack = NULL;  // Child doesn't inherit alternate stack
    child->in_signal_handler = 0;
    
    // Copy signal handlers
    for (int i = 0; i < 32; i++) {
        child->signal_handlers[i] = parent->signal_handlers[i];
    }
    
    spinlock_release_irqrestore(&child->signal_lock, child_flags);
    spinlock_release_irqrestore(&parent->signal_lock, parent_flags);
    
    serial_printf("[Signal] Copied signal handlers from PID %u to PID %u\n", 
                  parent->pid, child->pid);
}

//============================================================================
// Signal Registration
//============================================================================

void* signal_register_handler(pcb_t *proc, int signal, void *handler, uint32_t flags) {
    if (!proc || signal <= 0 || signal >= SIGNAL_MAX) {
        return SIG_ERR;
    }
    
    // SIGKILL and SIGSTOP cannot be caught or ignored
    if (signal == SIGKILL || signal == SIGSTOP) {
        return SIG_ERR;
    }
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->signal_lock);
    
    void *old_handler = proc->signal_handlers[signal - 1];
    proc->signal_handlers[signal - 1] = handler;
    
    // Update signal flags if provided
    if (flags != 0) {
        proc->signal_flags = flags;
    }
    
    spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
    
    serial_printf("[Signal] Registered handler %p for signal %d in PID %u\n", 
                  handler, signal, proc->pid);
    
    return old_handler;
}

//============================================================================
// Signal Sending
//============================================================================

int signal_send(uint32_t target_pid, int signal, uint32_t sender_pid) {
    if (signal <= 0 || signal >= SIGNAL_MAX) {
        return -EINVAL;
    }
    
    // Find target process
    pcb_t *target = process_get_by_pid(target_pid);
    if (!target) {
        return -ESRCH;  // No such process
    }
    
    // Permission check: only allow sending to own processes or if privileged
    pcb_t *sender = process_get_by_pid(sender_pid);
    if (sender && sender_pid != 0) {  // 0 = kernel sender
        if (target->pid != sender->pid && target->ppid != sender->pid) {
            // Simplified permission check - in real system would check UIDs
            return -EPERM;
        }
    }
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&target->signal_lock);
    
    // Special handling for SIGKILL and SIGSTOP - cannot be blocked or ignored
    if (signal == SIGKILL || signal == SIGSTOP) {
        target->pending_signals |= SIGMASK(signal);
        spinlock_release_irqrestore(&target->signal_lock, irq_flags);
        
        // Immediate termination for SIGKILL
        if (signal == SIGKILL) {
            signal_terminate_process(target, signal);
        }
        
        serial_printf("[Signal] Sent uncatchable signal %d to PID %u\n", signal, target_pid);
        return 0;
    }
    
    // Check if signal is ignored
    void *handler = target->signal_handlers[signal - 1];
    if (handler == SIG_IGN) {
        spinlock_release_irqrestore(&target->signal_lock, irq_flags);
        return 0;  // Signal ignored, but return success
    }
    
    // Set pending bit
    target->pending_signals |= SIGMASK(signal);
    
    spinlock_release_irqrestore(&target->signal_lock, irq_flags);
    
    // Wake up the target process if it's sleeping
    if (target->state == PROC_SLEEPING) {
        // TODO: Implement process wakeup - convert to scheduler's task representation
        serial_printf("[Signal] Target process PID %u is sleeping, should wake up\n", target_pid);
    }
    
    serial_printf("[Signal] Sent signal %d to PID %u from PID %u\n", 
                  signal, target_pid, sender_pid);
    
    return 0;
}

//============================================================================
// Signal Delivery
//============================================================================

bool signal_deliver_pending(pcb_t *proc, isr_frame_t *regs) {
    if (!proc || !regs) return false;
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->signal_lock);
    
    // Check if already in signal handler to prevent nested signals
    if (proc->in_signal_handler) {
        spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
        return false;
    }
    
    // Find highest priority pending, unblocked signal
    uint32_t deliverable = proc->pending_signals & ~proc->signal_mask;
    if (deliverable == 0) {
        spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
        return false;
    }
    
    // Find first set bit (lowest signal number has highest priority)
    int signal = 0;
    for (int i = 1; i < SIGNAL_MAX; i++) {
        if (deliverable & SIGMASK(i)) {
            signal = i;
            break;
        }
    }
    
    if (signal == 0) {
        spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
        return false;
    }
    
    // Clear pending bit
    proc->pending_signals &= ~SIGMASK(signal);
    
    void *handler = proc->signal_handlers[signal - 1];
    
    // Handle default actions
    if (handler == SIG_DFL) {
        spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
        
        int action = signal_default_action(signal);
        switch (action) {
            case 0: // Ignore
                break;
            case 1: // Terminate
            case 4: // Coredump (simplified as terminate)
                signal_terminate_process(proc, signal);
                break;
            case 2: // Stop
                proc->state = PROC_SLEEPING;  // Use available state
                // TODO: Send SIGCHLD to parent
                break;
            case 3: // Continue
                if (proc->state == PROC_SLEEPING) {
                    proc->state = PROC_READY;
                    // TODO: Add back to scheduler
                }
                break;
        }
        return action != 0;
    }
    
    // Handle SIG_IGN
    if (handler == SIG_IGN) {
        spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
        return false;
    }
    
    // Execute user signal handler
    proc->in_signal_handler = signal;
    spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
    
    // Set up user space signal handler execution
    // Save current context on user stack
    uint32_t user_esp = regs->useresp;
    
    // Allocate space for signal context on user stack
    user_esp -= sizeof(signal_context_t);
    signal_context_t *sig_ctx = (signal_context_t*)user_esp;
    
    // Fill signal context (simplified - would need proper user space writing)
    sig_ctx->eax = regs->eax;
    sig_ctx->ebx = regs->ebx;
    sig_ctx->ecx = regs->ecx;
    sig_ctx->edx = regs->edx;
    sig_ctx->esi = regs->esi;
    sig_ctx->edi = regs->edi;
    sig_ctx->ebp = regs->ebp;
    sig_ctx->esp = regs->useresp;
    sig_ctx->eip = regs->eip;
    sig_ctx->eflags = regs->eflags;
    sig_ctx->original_esp = regs->useresp;
    sig_ctx->signal_number = signal;
    
    // Set up signal handler call
    user_esp -= sizeof(uint32_t);  // Space for signal number argument
    *((uint32_t*)user_esp) = signal;
    
    user_esp -= sizeof(uint32_t);  // Space for return address (stub for now)
    *((uint32_t*)user_esp) = 0;  // Simplified - no return trampoline for now
    
    // Modify registers to call signal handler
    regs->useresp = user_esp;
    regs->eip = (uint32_t)handler;
    
    serial_printf("[Signal] Delivering signal %d to PID %u, handler=%p\n", 
                  signal, proc->pid, handler);
    
    return true;
}

//============================================================================
// Signal Mask Management
//============================================================================

int signal_mask_change(pcb_t *proc, int how, uint32_t newmask, uint32_t *oldmask) {
    if (!proc) return -EINVAL;
    
    // SIGKILL and SIGSTOP cannot be blocked
    newmask &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&proc->signal_lock);
    
    if (oldmask) {
        *oldmask = proc->signal_mask;
    }
    
    switch (how) {
        case SIG_BLOCK:
            proc->signal_mask |= newmask;
            break;
        case SIG_UNBLOCK:
            proc->signal_mask &= ~newmask;
            break;
        case SIG_SETMASK:
            proc->signal_mask = newmask;
            break;
        default:
            spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
            return -EINVAL;
    }
    
    spinlock_release_irqrestore(&proc->signal_lock, irq_flags);
    
    return 0;
}

//============================================================================
// Signal Termination
//============================================================================

void signal_terminate_process(pcb_t *proc, int signal) {
    if (!proc) return;
    
    serial_printf("[Signal] Terminating PID %u due to signal %d\n", proc->pid, signal);
    
    // Set exit status with signal information
    proc->exit_status = 128 + signal;  // Standard Unix convention
    proc->has_exited = true;
    proc->state = PROC_ZOMBIE;
    
    // TODO: Send SIGCHLD to parent
    if (proc->parent) {
        signal_send(proc->parent->pid, SIGCHLD, 0);
    }
    
    // Remove from scheduler
    // scheduler_remove_task(proc);  // Would need to implement
}

//============================================================================
// Signal Default Action Lookup
//============================================================================

int signal_default_action(int signal) {
    if (signal <= 0 || signal >= SIGNAL_MAX) {
        return 0;  // Ignore invalid signals
    }
    return signal_default_actions[signal];
}

//============================================================================
// Signal Integration with Hardware Interrupts
//============================================================================

void signal_handle_keyboard_interrupt(void) {
    // Send SIGINT to foreground process group
    // For now, send to all processes (simplified)
    pcb_t *current = get_current_process();
    if (current) {
        signal_send(current->pid, SIGINT, 0);
    }
}

void signal_handle_timer_signals(void) {
    // TODO: Implement timer-based signal delivery (SIGALRM)
    // This would be called from the timer interrupt handler
}

void signal_handle_page_fault(pcb_t *proc, uintptr_t fault_addr, uint32_t error_code) {
    if (!proc) return;
    
    // Send SIGSEGV for page faults that indicate invalid memory access
    if (error_code & 0x1) {  // Protection violation
        signal_send(proc->pid, SIGSEGV, 0);
    } else {
        // Page not present - might be valid, handle through page fault handler
    }
}

void signal_handle_fpe(pcb_t *proc) {
    if (!proc) return;
    
    signal_send(proc->pid, SIGFPE, 0);
}

//============================================================================
// Signal Return Mechanism (for returning from signal handlers)
//============================================================================

// This would be implemented as a small assembly trampoline
// that user space signal handlers return to
extern void signal_return_trampoline(void);

// System call to return from signal handler
int sys_sigreturn(signal_context_t *context) {
    pcb_t *proc = get_current_process();
    if (!proc || !context) {
        return -EINVAL;
    }
    
    // Restore original context
    // This would restore the register state from before the signal
    proc->in_signal_handler = 0;
    
    // Restore registers (would be done by assembly code)
    // restore_user_context(context);
    
    return 0;
}