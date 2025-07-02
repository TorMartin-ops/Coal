#ifndef SIGNAL_H
#define SIGNAL_H

#include <libc/stdint.h>
#include <libc/stdbool.h>
#include <kernel/process/process.h>
#include <kernel/cpu/isr_frame.h>

//============================================================================
// POSIX Signal Numbers
//============================================================================

#define SIGHUP    1     // Hangup detected on controlling terminal or death of controlling process
#define SIGINT    2     // Interrupt from keyboard (Ctrl+C)
#define SIGQUIT   3     // Quit from keyboard (Ctrl+\)
#define SIGILL    4     // Illegal Instruction
#define SIGTRAP   5     // Trace/breakpoint trap
#define SIGABRT   6     // Abort signal from abort()
#define SIGBUS    7     // Bus error (bad memory access)
#define SIGFPE    8     // Floating point exception
#define SIGKILL   9     // Kill signal (cannot be caught or ignored)
#define SIGUSR1   10    // User-defined signal 1
#define SIGSEGV   11    // Invalid memory reference
#define SIGUSR2   12    // User-defined signal 2
#define SIGPIPE   13    // Broken pipe: write to pipe with no readers
#define SIGALRM   14    // Timer signal from alarm()
#define SIGTERM   15    // Termination signal
#define SIGSTKFLT 16    // Stack fault on coprocessor (unused)
#define SIGCHLD   17    // Child stopped or terminated
#define SIGCONT   18    // Continue if stopped
#define SIGSTOP   19    // Stop process (cannot be caught or ignored)
#define SIGTSTP   20    // Stop typed at terminal (Ctrl+Z)
#define SIGTTIN   21    // Terminal input for background process
#define SIGTTOU   22    // Terminal output for background process
#define SIGURG    23    // Urgent condition on socket
#define SIGXCPU   24    // CPU time limit exceeded
#define SIGXFSZ   25    // File size limit exceeded
#define SIGVTALRM 26    // Virtual alarm clock
#define SIGPROF   27    // Profiling timer expired
#define SIGWINCH  28    // Window resize signal
#define SIGIO     29    // I/O now possible (synonym: SIGPOLL)
#define SIGPWR    30    // Power failure (System V)
#define SIGSYS    31    // Bad system call (SVr4)

#define SIGNAL_MAX 32   // Maximum signal number + 1

//============================================================================
// Signal Handler Constants
//============================================================================

#define SIG_DFL     ((void*)0)     // Default signal handler
#define SIG_IGN     ((void*)1)     // Ignore signal
#define SIG_ERR     ((void*)-1)    // Error return from signal()

//============================================================================
// Signal Action Flags (for sigaction)
//============================================================================

#define SA_NOCLDSTOP  0x00000001   // Don't send SIGCHLD when children stop
#define SA_NOCLDWAIT  0x00000002   // Don't create zombie on child death
#define SA_SIGINFO    0x00000004   // Invoke signal-catching function with three arguments
#define SA_ONSTACK    0x08000000   // Execute on alternate signal stack
#define SA_RESTART    0x10000000   // Restart system calls on signal return
#define SA_NODEFER    0x40000000   // Don't mask this signal while executing handler
#define SA_RESETHAND  0x80000000   // Reset signal handler to SIG_DFL after one call

//============================================================================
// Signal Mask Manipulation Macros
//============================================================================

#define SIGMASK(sig)        (1U << ((sig) - 1))    // Convert signal number to bit mask
#define SIG_BLOCK           0                       // Block signals in mask
#define SIG_UNBLOCK         1                       // Unblock signals in mask
#define SIG_SETMASK         2                       // Set signal mask

//============================================================================
// Signal-related Structures
//============================================================================

// Signal information structure (simplified siginfo_t)
typedef struct {
    int si_signo;       // Signal number
    int si_errno;       // Error number (usually 0)
    int si_code;        // Signal code
    uint32_t si_pid;    // Process ID of sender
    uint32_t si_uid;    // User ID of sender
    void *si_addr;      // Memory location which caused fault
    int si_status;      // Exit value or signal number
    int si_band;        // Band event for SIGPOLL
} siginfo_t;

// Signal action structure (simplified sigaction)
typedef struct {
    void (*sa_handler)(int);                    // Signal handler function
    void (*sa_sigaction)(int, siginfo_t*, void*); // Advanced signal handler
    uint32_t sa_mask;                           // Additional signals to block
    uint32_t sa_flags;                          // Signal action flags
} sigaction_t;

// Signal context for user space signal handling
typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip, eflags;
    uint16_t cs, ds, es, fs, gs, ss;
    uint32_t original_esp;                      // ESP before signal handling
    uint32_t signal_number;                     // Signal that was delivered
} signal_context_t;

//============================================================================
// Signal Management Functions
//============================================================================

/**
 * @brief Initialize signal handling for a process
 * @param proc Process to initialize
 */
void signal_init_process(pcb_t *proc);

/**
 * @brief Copy signal handlers from parent to child (for fork)
 * @param parent Parent process
 * @param child Child process
 */
void signal_copy_handlers(pcb_t *parent, pcb_t *child);

/**
 * @brief Send a signal to a process
 * @param target_pid Target process ID
 * @param signal Signal number to send
 * @param sender_pid Process ID of sender (0 for kernel)
 * @return 0 on success, negative error code on failure
 */
int signal_send(uint32_t target_pid, int signal, uint32_t sender_pid);

/**
 * @brief Register a signal handler for a process
 * @param proc Process
 * @param signal Signal number
 * @param handler Handler function (SIG_DFL, SIG_IGN, or user function)
 * @param flags Signal action flags
 * @return Previous handler on success, SIG_ERR on failure
 */
void* signal_register_handler(pcb_t *proc, int signal, void *handler, uint32_t flags);

/**
 * @brief Check for pending signals and deliver them
 * @param proc Process to check
 * @param regs Register context (for modifying user space execution)
 * @return true if a signal was delivered, false otherwise
 */
bool signal_deliver_pending(pcb_t *proc, isr_frame_t *regs);

/**
 * @brief Block or unblock signals for a process
 * @param proc Process
 * @param how SIG_BLOCK, SIG_UNBLOCK, or SIG_SETMASK
 * @param newmask New signal mask
 * @param oldmask Previous signal mask (output)
 * @return 0 on success, negative error code on failure
 */
int signal_mask_change(pcb_t *proc, int how, uint32_t newmask, uint32_t *oldmask);

/**
 * @brief Handle process termination due to signal
 * @param proc Process being terminated
 * @param signal Signal that caused termination
 */
void signal_terminate_process(pcb_t *proc, int signal);

/**
 * @brief Check if a signal is blocked for a process
 * @param proc Process
 * @param signal Signal number
 * @return true if blocked, false if not blocked
 */
static inline bool signal_is_blocked(pcb_t *proc, int signal) {
    return (proc->signal_mask & SIGMASK(signal)) != 0;
}

/**
 * @brief Check if a signal is pending for a process
 * @param proc Process
 * @param signal Signal number
 * @return true if pending, false if not pending
 */
static inline bool signal_is_pending(pcb_t *proc, int signal) {
    return (proc->pending_signals & SIGMASK(signal)) != 0;
}

/**
 * @brief Get default action for a signal
 * @param signal Signal number
 * @return Default action (SIG_DFL, SIG_IGN, or termination)
 */
int signal_default_action(int signal);

//============================================================================
// Signal Integration with Interrupts
//============================================================================

/**
 * @brief Handle keyboard interrupt (Ctrl+C) and send SIGINT
 */
void signal_handle_keyboard_interrupt(void);

/**
 * @brief Handle timer-based signals (SIGALRM)
 */
void signal_handle_timer_signals(void);

/**
 * @brief Handle page fault as potential SIGSEGV
 * @param proc Process that caused page fault
 * @param fault_addr Address that caused the fault
 * @param error_code Page fault error code
 */
void signal_handle_page_fault(pcb_t *proc, uintptr_t fault_addr, uint32_t error_code);

/**
 * @brief Handle floating point exception as SIGFPE
 * @param proc Process that caused the exception
 */
void signal_handle_fpe(pcb_t *proc);

#endif // SIGNAL_H