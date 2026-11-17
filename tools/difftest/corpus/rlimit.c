/* Resource limits: getrlimit/setrlimit/prlimit64 and the limits that are enforced. */
#include "trace.h"
#include <sys/resource.h>
#include <sys/wait.h>

struct rl { unsigned long cur, max; };
static const char *lim(unsigned long v) { static char b[4][24]; static int i; i = (i + 1) & 3; if (v == ~0UL) return "INF"; snprintf(b[i], 24, "%lu", v); return b[i]; }

int main(void) {
    init();
    scratch("rlimit");
    struct rl r;
    static const struct { int id; const char *name; } all[] = {{RLIMIT_CPU, "CPU"}, {RLIMIT_FSIZE, "FSIZE"}, {RLIMIT_DATA, "DATA"}, {RLIMIT_STACK, "STACK"},
        {RLIMIT_CORE, "CORE"}, {RLIMIT_NOFILE, "NOFILE"}, {RLIMIT_AS, "AS"}, {RLIMIT_NPROC, "NPROC"}};
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++) {
        char l[40];
        snprintf(l, sizeof l, "getrlimit(%s)", all[i].name);
        long q = K(SYS_getrlimit, all[i].id, &r);
        tr(l, q, q == 0 ? "cur<=max=%s" : NULL, yn(r.cur <= r.max));
    }
    R("getrlimit(bad resource)", K(SYS_getrlimit, 99, &r));
    R("getrlimit(bad ptr)", K(SYS_getrlimit, RLIMIT_NOFILE, 8));
    R("setrlimit(bad resource)", K(SYS_setrlimit, 99, &r));
    r.cur = 100; r.max = 50;
    R("setrlimit(cur > max)", K(SYS_setrlimit, RLIMIT_CORE, &r));

    r.cur = 20; r.max = 20;
    R("setrlimit(NOFILE 20)", K(SYS_setrlimit, RLIMIT_NOFILE, &r));
    struct rl g;
    K(SYS_getrlimit, RLIMIT_NOFILE, &g);
    NOTE("NOFILE now %s/%s", lim(g.cur), lim(g.max));
    long opened = 0, q;
    while ((q = K(SYS_open, "/etc/passwd", O_RDONLY)) >= 0) opened++;
    R("open until limit (EMFILE)", q);
    NOTE("descriptors opened beyond 0,1,2: %ld", opened);
    R("dup(at limit)", K(SYS_dup, 0));
    R("dup2(0, 25 beyond limit)", K(SYS_dup2, 0, 25));
    R("fcntl(F_DUPFD, 30 beyond limit)", K(SYS_fcntl, 0, F_DUPFD, 30));
    R("pipe(at limit)", ({ int p[2]; K(SYS_pipe, p); }));
    R("socket(at limit)", K(SYS_socket, AF_INET, SOCK_STREAM, 0));
    for (int i = 3; i < 20; i++) K(SYS_close, i);
    RFD("open(after closing)", K(SYS_open, "/etc/passwd", O_RDONLY));
    r.cur = 64; r.max = 64;
    R("setrlimit(NOFILE raise to 64 equal max? no, above max 20)", K(SYS_setrlimit, RLIMIT_NOFILE, &r));
    r.cur = 10; r.max = 10;
    R("setrlimit(lower both)", K(SYS_setrlimit, RLIMIT_NOFILE, &r));

    /* prlimit64 */
    struct rl nw = {1 << 20, 1 << 20}, old = {0, 0};
    R("prlimit64(self, FSIZE set+get)", K(SYS_prlimit64, 0, RLIMIT_FSIZE, &nw, &old));
    NOTE("old FSIZE %s/%s", lim(old.cur), lim(old.max));
    R("prlimit64(get new)", K(SYS_prlimit64, 0, RLIMIT_FSIZE, 0, &old));
    NOTE("FSIZE %s/%s", lim(old.cur), lim(old.max));
    R("prlimit64(bad pid)", K(SYS_prlimit64, 99999, RLIMIT_FSIZE, 0, &old));
    R("prlimit64(bad resource)", K(SYS_prlimit64, 0, 99, 0, &old));
    R("prlimit64(own pid)", K(SYS_prlimit64, K(SYS_getpid), RLIMIT_FSIZE, 0, &old));

    /* FSIZE: writing past the limit raises SIGXFSZ or fails with EFBIG when ignored */
    ignsig(SIGXFSZ);
    nw.cur = 100; nw.max = 100;
    K(SYS_setrlimit, RLIMIT_FSIZE, &nw);
    long f = K(SYS_open, "big", O_RDWR | O_CREAT, 0644);
    R("write(within FSIZE 100)", K(SYS_write, f, "0123456789", 10));
    R("lseek(95)", K(SYS_lseek, f, 95, SEEK_SET));
    R("write(crosses FSIZE)", K(SYS_write, f, "0123456789", 10));
    R("write(at FSIZE)", K(SYS_write, f, "x", 1));
    R("ftruncate(beyond FSIZE)", K(SYS_ftruncate, f, 1000));
    FSTAT("fstat(size capped)", f);
    nw.cur = 1 << 30; nw.max = 1 << 30;
    R("setrlimit(FSIZE raise within max)", K(SYS_setrlimit, RLIMIT_FSIZE, &nw));

    /* AS: mmap beyond the address-space limit */
    long c = K(SYS_fork);
    if (c == 0) {
        struct rl as = {64UL << 20, 64UL << 20};
        K(SYS_setrlimit, RLIMIT_AS, &as);
        long m = K(SYS_mmap, 0, 128UL << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        long m2 = K(SYS_mmap, 0, 1UL << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        K(SYS_exit, (m == -ENOMEM ? 1 : 0) | (m2 > 0 ? 2 : 0));
    }
    int st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("RLIMIT_AS: big mmap ENOMEM(1) small mmap ok(2)", WEXITSTATUS(st));
    /* CPU limit kills a spinning process with SIGXCPU */
    c = K(SYS_fork);
    if (c == 0) {   /* bounded: if the limit is not enforced the child gives up after 4 s */
        struct rl cp = {1, 1};
        struct { long s, n; } t0, t1;
        K(SYS_setrlimit, RLIMIT_CPU, &cp);
        K(SYS_clock_gettime, CLOCK_MONOTONIC, &t0);
        do K(SYS_clock_gettime, CLOCK_MONOTONIC, &t1); while (t1.s - t0.s < 4);
        K(SYS_exit, 77);
    }
    st = 0; K(SYS_wait4, c, &st, 0, 0);
    NOTE("RLIMIT_CPU 1s ends the spinner: signaled=%s", yn(WIFSIGNALED(st)));
    /* NPROC is not checked for root; getpriority/setpriority round trip */
    R("getpriority(self)", K(SYS_getpriority, 0, 0));
    R("setpriority(self 5)", K(SYS_setpriority, 0, 0, 5));
    R("getpriority(after)", K(SYS_getpriority, 0, 0));
    R("setpriority(bad which)", K(SYS_setpriority, 9, 0, 5));
    R("setpriority(bad pid)", K(SYS_setpriority, 0, 99999, 5));
    return 0;
}
