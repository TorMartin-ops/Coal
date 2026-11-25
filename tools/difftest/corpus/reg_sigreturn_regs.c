/* Regression: a signal handler that interrupts user code returns through rt_sigreturn, which must restore
 * rcx and r11 as they were. A return by sysret loaded rcx = rip there and corrupted the live value
 * (seen as a garbage pid in a fork() caller). */
#include "trace.h"
#include <sys/wait.h>

static volatile int fired;
static void on_alarm(int sig) { (void)sig; fired = 1; }

int main(void) {
    init();
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, 0);
    for (int round = 0; round < 3; round++) {
        uint64_t rcx = 0x1122334455667788ull + round, r11 = 0x0102030405060708ull + round, got_rcx, got_r11;
        fired = 0;
        long me = K(SYS_getpid), c = K(SYS_fork);      /* the child signals the spinning parent */
        if (c == 0) { K(SYS_kill, me, SIGALRM); K(SYS_exit, 0); }
        __asm__ volatile("mov %2, %%rcx\n\tmov %3, %%r11\n"
                         "1:\tcmpl $0, %4\n\tje 1b\n"
                         "\tmov %%rcx, %0\n\tmov %%r11, %1\n"
                         : "=r"(got_rcx), "=r"(got_r11)
                         : "r"(rcx), "r"(r11), "m"(fired)
                         : "rcx", "r11", "memory");
        K(SYS_wait4, c, 0, 0, 0);
        NOTE("round %d: rcx %s r11 %s", round, yn(got_rcx == rcx), yn(got_r11 == r11));
    }
    return 0;
}
