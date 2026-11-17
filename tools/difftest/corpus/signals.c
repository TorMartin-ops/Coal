/* Signals: handlers, masks, pending, kill, default actions, handler arguments and nesting. */
#include "trace.h"
#include <sys/wait.h>

struct ksa { void *h; unsigned long flags; void *restorer; unsigned long mask; };
extern void restorer_(void);
__asm__(".text\nrestorer_:\n mov $15, %eax\n syscall\n");   /* rt_sigreturn */

static volatile int hits[65], last_si_signo, last_code_user, nested;
static void h1(int s) { hits[s]++; }
static void h_info(int s, siginfo_t *si, void *uc) { hits[s]++; last_si_signo = si->si_signo; last_code_user = si->si_code == SI_USER; }
static void h_nest(int s) {
    hits[s]++;
    long cur[1];
    K(SYS_rt_sigprocmask, SIG_BLOCK, 0, cur, 8);
    nested = (cur[0] >> (SIGUSR1 - 1)) & 1;     /* handled signal is blocked while its handler runs */
    K(SYS_kill, K(SYS_getpid), SIGUSR2);        /* raised while SIGUSR2 is blocked by sa_mask */
    hits[0] = hits[SIGUSR2];
}

static long act(int sig, void *h, unsigned long flags, unsigned long mask) {
    struct ksa a = {h, flags | 0x04000000UL, restorer_, mask};
    return K(SYS_rt_sigaction, sig, &a, 0, 8);
}

int main(void) {
    init();
    long self = K(SYS_getpid);
    R("rt_sigaction(SIGUSR1)", act(SIGUSR1, h1, 0, 0));
    R("kill(self,USR1)", K(SYS_kill, self, SIGUSR1));
    R("handler ran", hits[SIGUSR1]);
    R("rt_sigaction(SIGKILL)", act(SIGKILL, h1, 0, 0));
    R("rt_sigaction(SIGSTOP)", act(SIGSTOP, h1, 0, 0));
    R("rt_sigaction(sig 0)", act(0, h1, 0, 0));
    R("rt_sigaction(sig 65)", act(65, h1, 0, 0));
    R("rt_sigaction(bad size)", ({ struct ksa a = {h1, 0, restorer_, 0}; K(SYS_rt_sigaction, SIGUSR1, &a, 0, 4); }));
    { struct ksa old = {0}; R("rt_sigaction(query old)", K(SYS_rt_sigaction, SIGUSR1, 0, &old, 8)); NOTE("old handler is h1: %s", yn(old.h == (void *)h1)); }

    /* mask and pending */
    long m = 1L << (SIGUSR1 - 1), old = 0, pend = 0;
    R("rt_sigprocmask(BLOCK USR1)", K(SYS_rt_sigprocmask, SIG_BLOCK, &m, &old, 8));
    R("kill(blocked)", K(SYS_kill, self, SIGUSR1));
    R("handler not yet", hits[SIGUSR1]);
    R("rt_sigpending", K(SYS_rt_sigpending, &pend, 8));
    NOTE("pending has USR1: %s", yn((pend >> (SIGUSR1 - 1)) & 1));
    R("kill(blocked again, coalesces)", K(SYS_kill, self, SIGUSR1));
    R("rt_sigprocmask(UNBLOCK)", K(SYS_rt_sigprocmask, SIG_UNBLOCK, &m, &old, 8));
    NOTE("old mask had USR1: %s", yn((old >> (SIGUSR1 - 1)) & 1));
    R("handler ran once for two", hits[SIGUSR1]);
    R("rt_sigprocmask(bad how)", K(SYS_rt_sigprocmask, 9, &m, 0, 8));
    { long all = -1, o2 = 0; K(SYS_rt_sigprocmask, SIG_SETMASK, &all, &o2, 8); long now = 0; K(SYS_rt_sigprocmask, SIG_BLOCK, 0, &now, 8);
      NOTE("KILL/STOP never blocked: %s", yn(!((now >> (SIGKILL - 1)) & 1) && !((now >> (SIGSTOP - 1)) & 1)));
      K(SYS_rt_sigprocmask, SIG_SETMASK, &o2, 0, 8); }

    /* SA_SIGINFO */
    R("rt_sigaction(USR2 SIGINFO)", act(SIGUSR2, h_info, SA_SIGINFO, 0));
    R("kill(self,USR2)", K(SYS_kill, self, SIGUSR2));
    NOTE("siginfo signo=%d code is SI_USER=%s", last_si_signo, yn(last_code_user));

    /* nesting and sa_mask; SA_NODEFER absent means signal blocked inside its handler */
    R("rt_sigaction(USR1 nest)", act(SIGUSR1, h_nest, 0, 1UL << (SIGUSR2 - 1)));
    R("rt_sigaction(USR2 counts)", act(SIGUSR2, h1, 0, 0));
    int before = hits[SIGUSR1];
    hits[SIGUSR2] = 0;
    R("kill(self,USR1 nested)", K(SYS_kill, self, SIGUSR1));
    NOTE("nested: usr1 hit=%d usr1 blocked in handler=%d usr2 delivered inside handler=%d after=%d", hits[SIGUSR1] - before, nested, hits[0], hits[SIGUSR2]);

    /* ignoring and defaults */
    R("rt_sigaction(USR1 IGN)", act(SIGUSR1, (void *)1, 0, 0));
    R("kill(self ignored)", K(SYS_kill, self, SIGUSR1));
    R("kill(sig 0 exists)", K(SYS_kill, self, 0));
    R("kill(bad pid)", K(SYS_kill, 999999, 0));
    R("kill(bad sig)", K(SYS_kill, self, 65));
    R("tkill(self,sig 0)", K(SYS_tkill, self, 0));
    R("tgkill(self)", K(SYS_tgkill, self, self, SIGUSR2));
    R("tgkill(bad tid)", K(SYS_tgkill, self, 999999, SIGUSR2));

    /* default actions seen from the parent */
    int sigs[] = {SIGTERM, SIGINT, SIGHUP, SIGABRT, SIGSEGV, SIGFPE, SIGUSR1, SIGALRM, SIGPIPE, SIGKILL};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        long c = K(SYS_fork);
        if (c == 0) { act(SIGUSR1, (void *)0, 0, 0); K(SYS_kill, K(SYS_getpid), sigs[i]); K(SYS_exit, 55); }
        int st = -1;
        K(SYS_wait4, c, &st, 0, 0);
        char l[40];
        snprintf(l, sizeof l, "default action of signal %d", sigs[i]);
        NOTE("%s: signaled=%s sig=%d", l, yn(WIFSIGNALED(st)), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    { long c = K(SYS_fork);                 /* SIGCHLD ignored by default, child reaped normally */
      if (c == 0) { K(SYS_kill, K(SYS_getppid), SIGCHLD); K(SYS_exit, 0); }
      int st = 0; R("wait4(after child sent SIGCHLD)", K(SYS_wait4, c, &st, 0, 0) == c ? 0 : -1); }
    { long c = K(SYS_fork);                 /* handler inherited by fork, reset by exec not tested */
      if (c == 0) { hits[SIGUSR2] = 0; act(SIGUSR2, h1, 0, 0); K(SYS_kill, K(SYS_getpid), SIGUSR2); K(SYS_exit, hits[SIGUSR2]); }
      int st = 0; K(SYS_wait4, c, &st, 0, 0); R("child handler ran", WEXITSTATUS(st)); }

    /* signal wakes a sleeping child: pause returns EINTR */
    { long c = K(SYS_fork);
      if (c == 0) { act(SIGUSR2, h1, 0, 0); long r = K(SYS_pause); K(SYS_exit, r == -EINTR ? 1 : 0); }
      for (volatile int i = 0; i < 1000000; i++) ;
      struct { long s, n; } ts = {0, 50000000};
      K(SYS_nanosleep, &ts, 0);
      K(SYS_kill, c, SIGUSR2);
      int st = 0; K(SYS_wait4, c, &st, 0, 0); R("pause interrupted with EINTR", WEXITSTATUS(st)); }
    { long c = K(SYS_fork);                 /* sigsuspend */
      if (c == 0) { act(SIGUSR2, h1, 0, 0); long blk = 1L << (SIGUSR2 - 1); K(SYS_rt_sigprocmask, SIG_BLOCK, &blk, 0, 8);
        long none = 0; hits[SIGUSR2] = 0; long r = K(SYS_rt_sigsuspend, &none, 8); K(SYS_exit, (r == -EINTR ? 1 : 0) | (hits[SIGUSR2] ? 2 : 0)); }
      struct { long s, n; } ts = {0, 50000000};
      K(SYS_nanosleep, &ts, 0);
      K(SYS_kill, c, SIGUSR2);
      int st = 0; K(SYS_wait4, c, &st, 0, 0); R("rt_sigsuspend result bits", WEXITSTATUS(st)); }

    /* sigaltstack */
    { struct { void *sp; int flags; size_t size; } ss = {0, 0, 0}, o;
      static char stk[16384];
      R("sigaltstack(query)", K(SYS_sigaltstack, 0, &o));
      NOTE("initial flags SS_DISABLE=%s", yn(o.flags & 2));
      ss.sp = stk; ss.size = sizeof stk;
      R("sigaltstack(set)", K(SYS_sigaltstack, &ss, 0));
      ss.size = 100;
      R("sigaltstack(too small)", K(SYS_sigaltstack, &ss, 0)); }
    return 0;
}
