/**
 * @file syscall_signal.c
 * @brief Signal handlers: rt_sigaction, rt_sigprocmask, delivery of one
 *        pending signal to a user handler on an rt signal frame, rt_sigreturn.
 *
 * Delivery happens when a system call returns to user mode. Default actions
 * (and the decision to hold a signal) stay in kernel/process/signal.c, which
 * asks signal_disposition() below.
 */

#include "syscall_internal.h"
#include <kernel/lib/string.h>
#include <kernel/process/fpu.h>

#define KSIGSET_SIZE    8                       /* bytes in the kernel sigset_t */
#define UNBLOCKABLE     ((1u << SIGKILL) | (1u << SIGSTOP))
#define EFLAGS_USER     0x00000CD5u             /* CF PF AF ZF SF DF OF */
#define EFLAGS_IF       0x00000200u

/* Signal numbers are bit n of the internal masks; the user's sigset has bit n-1. */
static uint32_t mask_from_user(uint32_t m) { return (m << 1) & ~UNBLOCKABLE; }
static uint32_t mask_to_user(uint32_t m)   { return m >> 1; }

struct sigcontext {
    uint32_t gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, eflags, esp_at_signal, ss, fpstate, oldmask, cr2;
};

struct rt_sigframe {
    uint32_t pretcode;
    int32_t  sig;
    uint32_t pinfo, puc;
    uint8_t  info[128];                         /* siginfo_t: only si_signo is filled */
    struct {
        uint32_t uc_flags, uc_link;
        uint32_t ss_sp, ss_flags, ss_size;
        struct sigcontext sc;
        uint32_t sigmask[2];
    } uc;
    uint8_t  retcode[8];                        /* mov eax, __NR_rt_sigreturn; int 0x80 */
    uint8_t  fpstate[FPU_AREA_SIZE];            /* FXSAVE image of the interrupted code (sc.fpstate points here) */
};

int signal_disposition(struct pcb *p, int sig) {
    uint32_t h;

    if (sig == SIGKILL || sig == SIGSTOP) return SIGDISP_DEFAULT;
    h = p->sigact[sig].handler;
    if (h == SIG_IGN) return SIGDISP_IGNORE;
    return (p->sig_blocked & (1u << sig)) || h > SIG_IGN ? SIGDISP_HOLD : SIGDISP_DEFAULT;
}

long sc_rt_sigaction(uint32_t sig, uint32_t uact, uint32_t uoact, uint32_t size) {
    pcb_t *ps = get_current_process();
    struct linux_sigaction act;

    if (size != KSIGSET_SIZE || sig == 0 || sig >= NSIG) return -EINVAL;
    if (uact) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        if (copy_from_user(&act, (const void *)uact, sizeof(act)) != 0) return -EFAULT;
    }
    if (uoact) {
        struct sigact *o = &ps->sigact[sig];
        struct linux_sigaction old = { o->handler, o->flags, o->restorer, { o->mask, 0 } };
        if (copy_to_user((void *)uoact, &old, sizeof(old)) != 0) return -EFAULT;
    }
    if (uact) {
        ps->sigact[sig] = (struct sigact){ act.handler, act.flags, act.restorer, act.mask[0] };
        if (act.handler == SIG_IGN) get_current_process()->pending_signals &= ~(1u << sig);
    }
    return 0;
}

long sc_rt_sigprocmask(uint32_t how, uint32_t uset, uint32_t uoset, uint32_t size) {
    pcb_t *ps = get_current_process();
    uint32_t set[2] = {0, 0}, old = ps->sig_blocked;

    if (size != KSIGSET_SIZE) return -EINVAL;
    if (uset) {
        if (copy_from_user(set, (const void *)uset, sizeof(set)) != 0) return -EFAULT;
        uint32_t m = mask_from_user(set[0]);
        switch (how) {
        case SIG_BLOCK:   ps->sig_blocked |= m; break;
        case SIG_UNBLOCK: ps->sig_blocked &= ~m; break;
        case SIG_SETMASK: ps->sig_blocked = m; break;
        default:          return -EINVAL;
        }
    }
    if (uoset) {
        uint32_t out[2] = { mask_to_user(old), 0 };
        if (copy_to_user((void *)uoset, out, sizeof(out)) != 0) return -EFAULT;
    }
    return 0;
}

/* pselect6: run with the caller's temporary mask; the old one comes back when the call ends or the handler returns. */
long signal_mask_swap(uint32_t uset, uint32_t size) {
    pcb_t *p = get_current_process();
    uint32_t set[2];

    if (size != KSIGSET_SIZE) return -EINVAL;
    if (copy_from_user(set, (const void *)uset, sizeof(set)) != 0) return -EFAULT;
    p->sig_saved = p->sig_blocked;
    p->sig_restore = true;
    p->sig_blocked = mask_from_user(set[0]);
    return 0;
}

void signal_mask_restore(void) {
    pcb_t *p = get_current_process();

    if (p->sig_restore) {
        p->sig_blocked = p->sig_saved;
        p->sig_restore = false;
    }
}

void signal_deliver(isr_frame_t *regs, uint32_t restart_nr) {
    pcb_t *p = get_current_process();
    uint32_t ready;
    int sig;

    if (!(ready = p->pending_signals & ~p->sig_blocked)) return;
    for (sig = 1; sig < NSIG; sig++)
        if ((ready & (1u << sig)) && p->sigact[sig].handler > SIG_IGN) break;
    if (sig == NSIG) return;

    struct sigact act = p->sigact[sig];
    struct rt_sigframe f;

    if (restart_nr && (act.flags & SA_RESTART)) {   /* the saved context re-executes "int 0x80" with the same arguments */
        regs->eax = restart_nr;
        regs->eip -= 2;
    }
    memset(&f, 0, sizeof(f));

    uint32_t sp = ((regs->useresp - sizeof(f)) & ~0xFu) - 4;    /* esp + 4 is 16-byte aligned at entry */
    f.pretcode = (act.flags & SA_RESTORER) ? act.restorer : sp + offsetof(struct rt_sigframe, retcode);
    f.sig = sig;
    f.pinfo = sp + offsetof(struct rt_sigframe, info);
    f.puc = sp + offsetof(struct rt_sigframe, uc);
    *(int32_t *)f.info = sig;
    memcpy(f.retcode, (uint8_t[]){0xB8, SYS_RT_SIGRETURN, 0, 0, 0, 0xCD, 0x80}, 7);
    uint32_t old_mask = p->sig_restore ? p->sig_saved : p->sig_blocked;     /* what sigreturn restores */
    p->sig_restore = false;
    f.uc.sigmask[0] = mask_to_user(old_mask);
    f.uc.sc = (struct sigcontext){
        .gs = regs->gs, .fs = regs->fs, .es = regs->es, .ds = regs->ds,
        .edi = regs->edi, .esi = regs->esi, .ebp = regs->ebp, .esp = regs->useresp,
        .ebx = regs->ebx, .edx = regs->edx, .ecx = regs->ecx, .eax = regs->eax,
        .eip = regs->eip, .cs = regs->cs, .eflags = regs->eflags,
        .esp_at_signal = regs->useresp, .ss = regs->ss, .oldmask = mask_to_user(old_mask),
    };

    if (fpu_save_current()) {                   /* the handler may use the FPU: keep the interrupted state in the frame */
        memcpy(f.fpstate, fpu_area(p), FPU_AREA_SIZE);
        f.uc.sc.fpstate = sp + offsetof(struct rt_sigframe, fpstate);
    }
    p->pending_signals &= ~(1u << sig);
    if (copy_to_user((void *)sp, &f, sizeof(f)) != 0)
        process_exit(SIGSEGV);                  /* no usable stack: Linux forces SIGSEGV */

    p->sig_blocked |= mask_from_user(act.mask) | ((act.flags & SA_NODEFER) ? 0 : 1u << sig);
    if (act.flags & SA_RESETHAND) p->sigact[sig] = (struct sigact){0};
    regs->eip = act.handler;
    regs->useresp = sp;
    regs->eflags &= ~0x500u;                    /* DF and TF clear on handler entry */
}

long sc_rt_sigreturn(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t ebp_arg, isr_frame_t *regs) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)ebp_arg;
    pcb_t *ps = get_current_process();
    struct rt_sigframe f;
    uint32_t frame = regs->useresp - 4;         /* the handler's ret popped pretcode: esp is one word past the frame start */

    if (copy_from_user(&f, (const void *)frame, sizeof(f)) != 0)
        process_exit(SIGSEGV);

    struct sigcontext *sc = &f.uc.sc;
    regs->edi = sc->edi; regs->esi = sc->esi; regs->ebp = sc->ebp;
    regs->ebx = sc->ebx; regs->edx = sc->edx; regs->ecx = sc->ecx;
    regs->eax = sc->eax;
    regs->eip = sc->eip;
    regs->useresp = sc->esp;
    regs->eflags = (sc->eflags & EFLAGS_USER) | EFLAGS_IF | 0x2;
    ps->sig_blocked = mask_from_user(f.uc.sigmask[0]);
    if (sc->fpstate == frame + offsetof(struct rt_sigframe, fpstate))
        fpu_restore_current(f.fpstate);
    return regs->eax;                           /* the dispatcher stores this back into eax */
}
