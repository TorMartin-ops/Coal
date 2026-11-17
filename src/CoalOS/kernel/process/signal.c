/**
 * Signals: SIGCHLD/SIGCONT/SIGURG/SIGWINCH and the job control stops are
 * ignored by default, everything else terminates the target. A signal is a bit
 * in pending_signals; signals the target blocks or handles in user mode
 * (signal_disposition() == SIGDISP_HOLD) stay pending and are delivered by the
 * system call layer, the rest act when the target next returns to user mode.
 */
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/cpu/isr_frame.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/fs/vfs/fs_errno.h>

#define IGNORED_BY_DEFAULT (SIGBIT(SIGCHLD) | SIGBIT(SIGCONT) | SIGBIT(SIGURG) | SIGBIT(SIGWINCH) | \
                            SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU))

bool signal_pending(void) {
    pcb_t *p = get_current_process();
    return p && (p->pending_signals & ~p->sig_blocked);
}

void signal_return_to_user(isr_frame_t *regs, int restart_nr) {
    pcb_t *p = get_current_process();
    uint64_t pending = p->pending_signals;

    if (!pending) return;
    for (int sig = 1; pending && sig <= NSIG; sig++) {
        if ((pending & SIGBIT(sig)) && signal_disposition(p, sig) == SIGDISP_DEFAULT) {
            p->pending_signals &= ~SIGBIT(sig);
            asm volatile("sti");            /* exiting may block; interrupt and exception gates enter with IRQs off */
            process_exit(sig);              /* wait status of a signalled process is the signal number */
        }
    }
    signal_deliver(regs, restart_nr);
}

void signal_force(int sig) {
    pcb_t *p = get_current_process();

    if (p->sigact[sig].handler <= SIG_IGN || (p->sig_blocked & SIGBIT(sig))) {
        p->sigact[sig] = (struct sigact){0};
        p->sig_blocked &= ~SIGBIT(sig);
    }
    p->pending_signals |= SIGBIT(sig);
}

void signal_send(pcb_t *p, int sig) {
    int disp = signal_disposition(p, sig);

    if (p->task.state == TASK_ZOMBIE || disp == SIGDISP_IGNORE ||
        (disp == SIGDISP_DEFAULT && (SIGBIT(sig) & IGNORED_BY_DEFAULT))) return;
    p->pending_signals |= SIGBIT(sig);
    if (!(p->sig_blocked & SIGBIT(sig)))
        scheduler_wake(&p->task);           /* a blocked call returns -EINTR */
}

int process_kill(int pid, int sig) {
    pcb_t *me = get_current_process(), *p;
    int hits = 0;

    if (sig < 0 || sig > NSIG) return -EINVAL;
    if (pid == 0 && !me) return -ESRCH;
    if (pid > 0) {
        p = process_find((uint32_t)pid);
        if (!p) return -ESRCH;
        if (sig) signal_send(p, sig);
        return 0;
    }
    for (p = process_all(); p; p = p->all_next) {
        bool match = pid == -1 ? p->pid > 1 && p != me
                   : p->pgid == (pid == 0 ? me->pgid : (uint32_t)-pid);
        if (!match || p == me || !p->pid) continue;
        hits++;
        if (sig) signal_send(p, sig);
    }
    if (me && (pid == 0 || (pid < -1 && me->pgid == (uint32_t)-pid))) {
        hits++;
        if (sig) signal_send(me, sig);
    }
    return hits ? 0 : -ESRCH;
}
