/**
 * @file signal.h
 * @brief Linux i386 signal numbers. Only default actions are implemented.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include <kernel/core/types.h>

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGWINCH 28
#define NSIG     32

/* How the target treats a signal right now; answered by the system call layer. */
#define SIGDISP_DEFAULT 0       /* default action */
#define SIGDISP_IGNORE  1       /* SIG_IGN: drop it */
#define SIGDISP_HOLD    2       /* blocked or has a user handler: keep it pending */
struct pcb;
int signal_disposition(struct pcb *p, int sig);

/* Before returning to user mode: builds an rt signal frame for one pending signal that has a handler. */
struct isr_frame;
void signal_deliver(struct isr_frame *regs, uint32_t restart_nr);

/**
 * The one place signals take effect. Every return to ring 3 (system call exit,
 * interrupt or exception return) calls it: a pending signal with the default
 * action ends the process, one with a handler gets its frame. Kernel code is
 * never interrupted by a signal; blocking calls return -EINTR instead.
 * restart_nr is the number of a restartable system call that just returned -EINTR (0 otherwise):
 * if the handler that gets delivered has SA_RESTART, the call is re-issued after the handler returns.
 */
void signal_return_to_user(struct isr_frame *regs, uint32_t restart_nr);

/** pselect6: installs the user's sigset at uset as the blocked mask until signal_mask_restore() or a handler returns. */
long signal_mask_swap(uint32_t uset, uint32_t size);
/** Puts back the mask saved by signal_mask_swap() unless a handler frame took over that job. */
void signal_mask_restore(void);

/** True if a deliverable signal is pending for the current process (blocking calls give up). */
bool signal_pending(void);

/** Queues sig for p unless it is ignored or the target is a zombie. Safe from interrupt context. */
void signal_send(struct pcb *p, int sig);

/** Raises a CPU-fault signal on the current process; a blocked or ignored one is reset to the default action. */
void signal_force(int sig);

#endif
