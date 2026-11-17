/**
 * Signals: SIGCHLD/SIGCONT/SIGURG/SIGWINCH and the job control stops are
 * ignored by default, everything else terminates the target. A signal is a bit
 * in pending_signals; signals the target blocks or handles in user mode
 * (signal_disposition() == SIGDISP_HOLD) stay pending and are delivered by the
 * system call layer, the rest act the next time the target is scheduled.
 */
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/fs/vfs/fs_errno.h>

#define IGNORED_BY_DEFAULT ((1u << SIGCHLD) | (1u << SIGCONT) | (1u << SIGURG) | (1u << SIGWINCH) | \
                            (1u << SIGSTOP) | (1u << SIGTSTP) | (1u << SIGTTIN) | (1u << SIGTTOU))

void signal_dispatch_current(void) {
    pcb_t *p = get_current_process();
    uint32_t pending;

    if (!p || !(pending = p->pending_signals)) return;
    for (int sig = 1; sig < NSIG; sig++) {
        if ((pending & (1u << sig)) && signal_disposition(p, sig) == SIGDISP_DEFAULT) {
            p->pending_signals &= ~(1u << sig);
            process_exit(sig);              /* wait status of a signalled process is the signal number */
        }
    }
}

static void send_one(pcb_t *p, int sig) {
    int disp = signal_disposition(p, sig);

    if (p->task.state == TASK_ZOMBIE || disp == SIGDISP_IGNORE ||
        (disp == SIGDISP_DEFAULT && ((1u << sig) & IGNORED_BY_DEFAULT))) return;
    p->pending_signals |= 1u << sig;
    if (disp == SIGDISP_HOLD) return;
    if (p == get_current_process()) signal_dispatch_current();
    else scheduler_wake(&p->task);
}

int process_kill(int pid, int sig) {
    pcb_t *me = get_current_process(), *p;
    int hits = 0;

    if (sig < 0 || sig >= NSIG) return -EINVAL;
    if (pid == 0 && !me) return -ESRCH;
    if (pid > 0) {
        p = process_find((uint32_t)pid);
        if (!p) return -ESRCH;
        if (sig) send_one(p, sig);
        return 0;
    }
    for (p = process_all(); p; p = p->all_next) {
        bool match = pid == -1 ? p->pid > 1 && p != me
                   : p->pgid == (pid == 0 ? me->pgid : (uint32_t)-pid);
        if (!match || p == me || !p->pid) continue;
        hits++;
        if (sig) send_one(p, sig);
    }
    if (me && (pid == 0 || (pid < -1 && me->pgid == (uint32_t)-pid))) {
        hits++;
        if (sig) send_one(me, sig);       /* last: this may not return */
    }
    return hits ? 0 : -ESRCH;
}
