/* Regression: RLIMIT_CPU of 1 s soft / 2 s hard on a spinning child. The child must see SIGXCPU and
 * only then SIGKILL; the hard-limit event used to be lost when an earlier timer event consumed it, and
 * the late charge killed the child without a SIGXCPU. */
#include "trace.h"
#include <sys/resource.h>
#include <sys/wait.h>

static void on_xcpu(int sig) { (void)sig; static const char m[] = "child: SIGXCPU\n"; K(SYS_write, 1, m, sizeof m - 1); }

int main(void) {
    init();
    struct sigaction sa = {.sa_handler = on_xcpu};
    sigaction(SIGXCPU, &sa, 0);
    long sleeper = K(SYS_fork);                    /* its wake-up arms a timer event that is not the CPU limit's */
    if (sleeper == 0) {
        struct timespec brief = {0, 20 * 1000 * 1000}, long_ = {100, 0};
        K(SYS_nanosleep, &brief, 0);
        for (;;) K(SYS_nanosleep, &long_, 0);
    }
    for (int round = 0; round < 3; round++) {
        long c = K(SYS_fork);
        if (c == 0) {
            struct rlimit r = {1, 2};
            setrlimit(RLIMIT_CPU, &r);
            for (;;) ;
        }
        int st = 0;
        K(SYS_wait4, c, &st, 0, 0);
        NOTE("round %d: killed by signal %d", round, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    K(SYS_kill, sleeper, SIGKILL);
    K(SYS_wait4, sleeper, 0, 0, 0);
    return 0;
}
