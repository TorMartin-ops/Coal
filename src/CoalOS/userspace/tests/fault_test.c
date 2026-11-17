/* CPU exceptions raised in user mode must end only the offending process with
 * the Linux signal (wait status = signal number), or run the user's handler. */

#include <libc/stdio.h>
#include <libc/unistd.h>
#include <libc/signal.h>
#include <libc/sys/wait.h>
#include "check.h"

#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif

static void fault_ud2(void)  { asm volatile("ud2"); }
static void fault_noncanon(void) { *(volatile int *)0x8000000000000000ul = 1; }   /* #GP, not #PF */
static void fault_kernel(void) { *(volatile int *)0xffffffff80100000ul = 1; }  /* kernel text from ring 3 */
static void fault_cli(void)  { asm volatile("cli"); }                  /* privileged: #GP */
static void fault_int3(void) { asm volatile("int3"); }
static void fault_int81(void) { asm volatile("int $0x81"); }           /* no gate: #GP */
static void fault_div0(void) {
    asm volatile("xor %%ecx, %%ecx; div %%ecx" : : "a"(1), "d"(0) : "ecx");
}
static void fault_null(void) { *(volatile int *)0 = 1; }

static void on_signal(int sig) { _exit(40 + sig); }

/* Runs fn in a child; handler != NULL installs it for sig first. Returns the wait status. */
static int run_child(void (*fn)(void), int sig, sighandler_t handler) {
    int status = -1;
    pid_t pid = fork();

    if (pid == 0) {
        if (handler) signal(sig, handler);
        fn();
        _exit(0);                           /* the fault did not happen */
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return -1;
    return status;
}

static void expect_signal(const char *name, void (*fn)(void), int sig) {
    int st = run_child(fn, 0, NULL);
    int ok = WIFSIGNALED(st) && WTERMSIG(st) == sig;

    if (!ok) printf("%s: status %#x, expected signal %d\n", name, st, sig);
    CHECK(ok);
}

static void expect_handler(const char *name, void (*fn)(void), int sig) {
    int st = run_child(fn, sig, on_signal);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 40 + sig;

    if (!ok) printf("%s handler: status %#x, expected exit %d\n", name, st, 40 + sig);
    CHECK(ok);
}

int main(void) {
    expect_signal("ud2", fault_ud2, SIGILL);
    expect_signal("non-canonical", fault_noncanon, SIGSEGV);
    expect_signal("kernel address", fault_kernel, SIGSEGV);
    expect_signal("div0", fault_div0, SIGFPE);
    expect_signal("cli", fault_cli, SIGSEGV);
    expect_signal("int $0x81", fault_int81, SIGSEGV);
    expect_signal("int3", fault_int3, SIGTRAP);
    expect_signal("null", fault_null, SIGSEGV);
    expect_handler("ud2", fault_ud2, SIGILL);
    expect_handler("div0", fault_div0, SIGFPE);
    expect_handler("null", fault_null, SIGSEGV);

    /* The kernel survived: it still schedules and forks. */
    CHECK(run_child(fault_int3, 0, NULL) != -1);
    return check_report("fault_test");
}
