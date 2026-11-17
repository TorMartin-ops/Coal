/**
 * @file syscall_signal.c
 * @brief Signal handlers: rt_sigaction, rt_sigprocmask, sigaltstack, delivery of one pending signal to a
 *        user handler on an x86_64 rt signal frame, rt_sigreturn.
 *
 * Delivery happens when a system call returns to user mode. Default actions (and the decision to hold a
 * signal) stay in kernel/process/signal.c, which asks signal_disposition() below.
 */

#include "syscall_internal.h"
#include <kernel/lib/string.h>
#include <kernel/process/fpu.h>

#define KSIGSET_SIZE    8                       /* bytes in the kernel sigset_t */
#define UNBLOCKABLE     (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define EFLAGS_USER     0x00040DD5u             /* CF PF AF ZF SF TF DF OF AC */
#define EFLAGS_IF       0x00000200u
#define EFLAGS_ENTRY    0x00040500u             /* DF, TF and AC are cleared on handler entry */
#define RED_ZONE        128
#define SA_ONSTACK      0x08000000u
#define SS_ONSTACK      1
#define SS_DISABLE      2
#define MINSIGSTKSZ     2048

struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};

struct linux_stack { uintptr_t sp; int32_t flags, pad; size_t size; };

struct ucontext {
    uint64_t uc_flags, uc_link;
    struct linux_stack uc_stack;
    struct sigcontext sc;
    uint64_t sigmask;
};

struct rt_sigframe {
    uint64_t pretcode;                          /* the handler "returns" into the user's SA_RESTORER stub */
    struct ucontext uc;
    uint8_t  info[128];                         /* siginfo_t: only si_signo is filled */
};

_Static_assert(sizeof(struct sigcontext) == 256 && sizeof(struct ucontext) == 304, "x86_64 ucontext layout");

int signal_disposition(struct pcb *p, int sig) {
    uintptr_t h;

    if (sig == SIGKILL || sig == SIGSTOP) return SIGDISP_DEFAULT;
    h = p->sigact[sig].handler;
    if (h == SIG_IGN) return SIGDISP_IGNORE;
    return (p->sig_blocked & SIGBIT(sig)) || h > SIG_IGN ? SIGDISP_HOLD : SIGDISP_DEFAULT;
}

long sc_rt_sigaction(uint32_t sig, uintptr_t uact, uintptr_t uoact, size_t size) {
    pcb_t *ps = get_current_process();
    struct linux_sigaction act;

    if (size != KSIGSET_SIZE || sig == 0 || sig > NSIG) return -EINVAL;
    if (uact) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        if (copy_from_user(&act, (const void *)uact, sizeof(act)) != 0) return -EFAULT;
        if (act.handler > SIG_IGN && !(act.flags & SA_RESTORER)) return -EINVAL;   /* there is no default trampoline */
    }
    if (uoact && copy_to_user((void *)uoact, &ps->sigact[sig], sizeof(act)) != 0) return -EFAULT;
    if (uact) {
        ps->sigact[sig] = (struct sigact){ act.handler, act.flags, act.restorer, act.mask };
        if (act.handler == SIG_IGN) ps->pending_signals &= ~SIGBIT(sig);
    }
    return 0;
}

long sc_rt_sigprocmask(uint32_t how, uintptr_t uset, uintptr_t uoset, size_t size) {
    pcb_t *ps = get_current_process();
    uint64_t set = 0, old = ps->sig_blocked;

    if (size != KSIGSET_SIZE) return -EINVAL;
    if (uset) {
        if (copy_from_user(&set, (const void *)uset, sizeof(set)) != 0) return -EFAULT;
        set &= ~UNBLOCKABLE;
        switch (how) {
        case SIG_BLOCK:   ps->sig_blocked |= set; break;
        case SIG_UNBLOCK: ps->sig_blocked &= ~set; break;
        case SIG_SETMASK: ps->sig_blocked = set; break;
        default:          return -EINVAL;
        }
    }
    if (uoset && copy_to_user((void *)uoset, &old, sizeof(old)) != 0) return -EFAULT;
    return 0;
}

/* The alternate stack counts as in use while sp lies on it. */
static bool on_altstack(const pcb_t *p, uintptr_t sp) {
    return p->sigaltstack_sp && sp > p->sigaltstack_sp && sp <= p->sigaltstack_sp + p->sigaltstack_size;
}

long sc_sigaltstack(uintptr_t unew, uintptr_t uold) {
    pcb_t *p = get_current_process();
    isr_frame_t *regs = user_regs();
    struct linux_stack ss;

    if (uold) {
        struct linux_stack old = { p->sigaltstack_sp, p->sigaltstack_sp ? (on_altstack(p, regs->rsp) ? SS_ONSTACK : 0) : SS_DISABLE, 0,
                                   p->sigaltstack_size };
        if (copy_to_user((void *)uold, &old, sizeof(old)) != 0) return -EFAULT;
    }
    if (!unew) return 0;
    if (copy_from_user(&ss, (const void *)unew, sizeof(ss)) != 0) return -EFAULT;
    if (on_altstack(p, regs->rsp)) return -EPERM;
    if (ss.flags & SS_DISABLE) {
        p->sigaltstack_sp = 0;
        p->sigaltstack_size = 0;
        return 0;
    }
    if (ss.size < MINSIGSTKSZ) return -ENOMEM;
    p->sigaltstack_sp = ss.sp;
    p->sigaltstack_size = ss.size;
    return 0;
}

/* pselect6/ppoll: run with the caller's temporary mask; the old one comes back when the call ends or the handler returns. */
long signal_mask_swap(uintptr_t uset, size_t size) {
    pcb_t *p = get_current_process();
    uint64_t set;

    if (size != KSIGSET_SIZE) return -EINVAL;
    if (copy_from_user(&set, (const void *)uset, sizeof(set)) != 0) return -EFAULT;
    p->sig_saved = p->sig_blocked;
    p->sig_restore = true;
    p->sig_blocked = set & ~UNBLOCKABLE;
    return 0;
}

void signal_mask_restore(void) {
    pcb_t *p = get_current_process();

    if (p->sig_restore) {
        p->sig_blocked = p->sig_saved;
        p->sig_restore = false;
    }
}

void signal_deliver(isr_frame_t *regs, int restart_nr) {
    pcb_t *p = get_current_process();
    uint64_t ready;
    int sig;

    if (!(ready = p->pending_signals & ~p->sig_blocked)) return;
    for (sig = 1; sig <= NSIG; sig++)
        if ((ready & SIGBIT(sig)) && p->sigact[sig].handler > SIG_IGN) break;
    if (sig > NSIG) return;

    struct sigact act = p->sigact[sig];
    struct rt_sigframe f;

    if (restart_nr != SIGNAL_NO_RESTART && (act.flags & SA_RESTART)) {   /* the saved context re-executes "syscall" with the same arguments */
        regs->rax = restart_nr;
        regs->rip -= 2;
    }
    memset(&f, 0, sizeof(f));

    /* Below the red zone, or on the alternate stack; the FXSAVE image sits above the frame. */
    uintptr_t top = regs->rsp - RED_ZONE;
    if ((act.flags & SA_ONSTACK) && p->sigaltstack_sp && !on_altstack(p, regs->rsp))
        top = p->sigaltstack_sp + p->sigaltstack_size;
    uintptr_t fp = (top - FPU_AREA_SIZE) & ~(uintptr_t)63;
    uintptr_t sp = ((fp - sizeof(f)) & ~(uintptr_t)15) - 8;     /* rsp + 8 is 16-byte aligned at handler entry */

    f.pretcode = act.restorer;
    *(int32_t *)f.info = sig;
    uint64_t old_mask = p->sig_restore ? p->sig_saved : p->sig_blocked;     /* what sigreturn restores */
    p->sig_restore = false;
    f.uc.sigmask = old_mask;
    f.uc.uc_stack = (struct linux_stack){ p->sigaltstack_sp, p->sigaltstack_sp ? (on_altstack(p, regs->rsp) ? SS_ONSTACK : 0) : SS_DISABLE, 0,
                                          p->sigaltstack_size };
    f.uc.sc = (struct sigcontext){
        .r8 = regs->r8, .r9 = regs->r9, .r10 = regs->r10, .r11 = regs->r11, .r12 = regs->r12, .r13 = regs->r13,
        .r14 = regs->r14, .r15 = regs->r15, .rdi = regs->rdi, .rsi = regs->rsi, .rbp = regs->rbp, .rbx = regs->rbx,
        .rdx = regs->rdx, .rax = regs->rax, .rcx = regs->rcx, .rsp = regs->rsp, .rip = regs->rip,
        .eflags = regs->rflags, .cs = regs->cs, .ss = regs->ss, .oldmask = old_mask,
    };

    bool have_fpu = fpu_save_current();         /* the handler may use the FPU: keep the interrupted state in the frame */
    if (have_fpu) f.uc.sc.fpstate = fp;
    p->pending_signals &= ~SIGBIT(sig);
    if (copy_to_user((void *)sp, &f, sizeof(f)) != 0 ||
        (have_fpu && copy_to_user((void *)fp, fpu_area(p), FPU_AREA_SIZE) != 0))
        process_exit(SIGSEGV);                  /* no usable stack: Linux forces SIGSEGV */

    p->sig_blocked |= (act.mask | ((act.flags & SA_NODEFER) ? 0 : SIGBIT(sig))) & ~UNBLOCKABLE;
    if (act.flags & SA_RESETHAND) p->sigact[sig] = (struct sigact){0};
    regs->rip = act.handler;
    regs->rsp = sp;
    regs->rdi = sig;
    regs->rsi = sp + offsetof(struct rt_sigframe, info);
    regs->rdx = sp + offsetof(struct rt_sigframe, uc);
    regs->rax = 0;
    regs->rflags &= ~(uint64_t)EFLAGS_ENTRY;
}

long sc_rt_sigreturn(void) {
    pcb_t *ps = get_current_process();
    isr_frame_t *regs = user_regs();
    struct rt_sigframe f;
    uintptr_t frame = regs->rsp - 8;            /* the handler's ret popped pretcode */

    if (copy_from_user(&f, (const void *)frame, sizeof(f)) != 0)
        process_exit(SIGSEGV);

    struct sigcontext *sc = &f.uc.sc;
    regs->r8 = sc->r8; regs->r9 = sc->r9; regs->r10 = sc->r10; regs->r11 = sc->r11;
    regs->r12 = sc->r12; regs->r13 = sc->r13; regs->r14 = sc->r14; regs->r15 = sc->r15;
    regs->rdi = sc->rdi; regs->rsi = sc->rsi; regs->rbp = sc->rbp; regs->rbx = sc->rbx;
    regs->rdx = sc->rdx; regs->rax = sc->rax; regs->rcx = sc->rcx;
    regs->rip = sc->rip;
    regs->rsp = sc->rsp;
    regs->rflags = (sc->eflags & EFLAGS_USER) | EFLAGS_IF | 0x2;
    ps->sig_blocked = f.uc.sigmask & ~UNBLOCKABLE;
    if (sc->fpstate) {
        uint8_t fx[FPU_AREA_SIZE] __attribute__((aligned(16)));
        if (copy_from_user(fx, (const void *)sc->fpstate, sizeof(fx)) == 0) fpu_restore_current(fx);
    }
    return regs->rax;                           /* the dispatcher stores this back into rax */
}
