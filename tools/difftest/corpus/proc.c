/* Processes: fork, exit codes, wait4 status and options, process groups, exec. */
#include "trace.h"
#include <sys/wait.h>

static void show(const char *label, long r, long expect, int st) {
    if (r != expect) { tr(label, r, NULL); return; }
    NOTE("%s: reaped=expected exited=%s code=%d signaled=%s sig=%d", label, yn(WIFEXITED(st)),
         WIFEXITED(st) ? WEXITSTATUS(st) : -1, yn(WIFSIGNALED(st)), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

#define WS(label, pidexpr, opts) do { long p_ = (pidexpr); st = -1; long r_ = K(SYS_wait4, p_, &st, opts, 0); show(label, r_, p_ == -1 ? r_ : p_, st); } while (0)

int main(int argc, char **argv) {
    if (argc > 1) {                     /* exec target: report argv/environment, exit with code */
        NOTE("child argc=%d argv1=%s argv2=%s env=%s", argc, argv[1], argc > 2 ? argv[2] : "-", getenv("DT_VAR") ? getenv("DT_VAR") : "unset");
        return 42;
    }
    init();
    int st;
    long pid = R("getpid>0", K(SYS_getpid) > 0 ? 0 : -1);
    (void)pid;
    R("wait4(no children)", K(SYS_wait4, -1, &st, 0, 0));
    R("wait4(bad pid)", K(SYS_wait4, 99999, &st, 0, 0));
    R("wait4(bad options)", K(SYS_wait4, -1, &st, 0x40000000, 0));

    long c = K(SYS_fork);
    if (c == 0) K(SYS_exit_group, 3);
    NOTE("fork: child=%s", pidn(c));
    st = -1;
    WS("wait4(exit 3)", c, 0);
    R("wait4(again)", K(SYS_wait4, c, &st, 0, 0));

    c = K(SYS_fork);
    if (c == 0) K(SYS_exit, 255);
    st = -1;
    WS("wait4(exit 255)", -1, 0);

    c = K(SYS_fork);
    if (c == 0) { for (;;) K(SYS_pause); }
    R("wait4(WNOHANG running)", K(SYS_wait4, c, &st, WNOHANG, 0));
    R("kill(child TERM)", K(SYS_kill, c, SIGTERM));
    st = -1;
    WS("wait4(killed)", c, 0);

    c = K(SYS_fork);
    if (c == 0) { volatile int *p = 0; *p = 1; K(SYS_exit, 0); }
    st = -1;
    WS("wait4(segv child)", c, 0);

    c = K(SYS_fork);
    if (c == 0) K(SYS_exit, 9);
    long c2 = K(SYS_fork);
    if (c2 == 0) K(SYS_exit, 8);
    int seen = 0;
    for (int i = 0; i < 2; i++) { st = 0; long w = K(SYS_wait4, -1, &st, 0, 0); seen += (w == c || w == c2) ? (WEXITSTATUS(st)) : 1000; }
    R("wait4(two children exit codes sum)", seen);
    R("wait4(none left)", K(SYS_wait4, -1, &st, 0, 0));

    /* many children */
    int ok = 0;
    for (int i = 0; i < 8; i++) { c = K(SYS_fork); if (c == 0) K(SYS_exit, i); if (c > 0) { st = 0; K(SYS_wait4, c, &st, 0, 0); ok += WEXITSTATUS(st) == i; } }
    R("fork(8 sequential)", ok);

    /* memory is copied */
    static int g = 1;
    c = K(SYS_fork);
    if (c == 0) { g = 2; K(SYS_exit, g); }
    st = 0; K(SYS_wait4, c, &st, 0, 0);
    NOTE("fork copy: parent g=%d child exit=%d", g, WEXITSTATUS(st));

    /* ids */
    long pg = K(SYS_getpgid, 0);
    R("getpgid(0)>0", pg > 0 ? 0 : -1);
    R("getpgid(bad)", K(SYS_getpgid, 99999));
    R("getsid(0)>0", K(SYS_getsid, 0) > 0 ? 0 : -1);
    c = K(SYS_fork);
    if (c == 0) {
        long s = K(SYS_setsid);
        long ppid = K(SYS_getppid);
        K(SYS_exit, (s > 0 ? 1 : 0) | (ppid > 0 ? 2 : 0) | (K(SYS_getpgid, 0) == s ? 4 : 0));
    }
    st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("child setsid/getppid/getpgid bits", WEXITSTATUS(st));
    R("setpgid(0,0)", K(SYS_setpgid, 0, 0));
    R("setpgid(bad pid)", K(SYS_setpgid, 99999, 0));

    /* exec */
    char *args[] = {argv[0], "one", "two", 0};
    char *env[] = {"DT_VAR=value", 0};
    c = K(SYS_fork);
    if (c == 0) { K(SYS_execve, argv[0], args, env); K(SYS_exit, 99); }
    st = 0; WS("wait4(exec child exit 42)", c, 0);
    c = K(SYS_fork);
    if (c == 0) { long e = K(SYS_execve, "/nonexistent", args, env); K(SYS_exit, (int)-e); }
    st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("execve(missing) errno", -(long)WEXITSTATUS(st));
    c = K(SYS_fork);
    if (c == 0) { long e = K(SYS_execve, "/tmp", args, env); K(SYS_exit, (int)-e); }
    st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("execve(dir) errno", -(long)WEXITSTATUS(st));
    c = K(SYS_fork);
    if (c == 0) { long e = K(SYS_execve, "/etc/passwd", args, env); K(SYS_exit, (int)-e); }
    st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("execve(not executable) errno", -(long)WEXITSTATUS(st));
    return 0;
}
