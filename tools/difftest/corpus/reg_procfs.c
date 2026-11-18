/* Regression: /proc/self/stat ids, /proc/cpuinfo, /proc/sys/kernel/pid_max and the Uid line of status. */
#include "trace.h"

static long slurp(const char *path, char *buf, long n) {
    long fd = K(SYS_open, path, O_RDONLY);
    if (fd < 0) return fd;
    long r = K(SYS_read, fd, buf, n - 1);
    K(SYS_close, fd);
    if (r >= 0) buf[r] = 0;
    return r;
}

static int atoi_(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return v; }

int main(void) {
    init();
    char b[1024];
    long r = slurp("/proc/self/stat", b, sizeof(b));
    R("open stat", r < 0 ? r : 0);
    if (r > 0) {
        char *p = strrchr(b, ')') + 2;
        int ppid = atoi_(p + 2);
        p += 2; while (*p != ' ') p++;
        int pgrp = atoi_(p + 1);
        NOTE("stat pid %s, ppid %s, pgrp %s", yn(atoi_(b) == K(SYS_getpid)), yn(ppid == K(SYS_getppid)), yn(pgrp == K(SYS_getpgrp)));
    }
    r = slurp("/proc/cpuinfo", b, sizeof(b));
    R("open cpuinfo", r < 0 ? r : 0);
    NOTE("cpuinfo lists a processor: %s", yn(r > 0 && strstr(b, "processor")));
    r = slurp("/proc/sys/kernel/pid_max", b, sizeof(b));
    R("open pid_max", r < 0 ? r : 0);
    NOTE("pid_max is a positive number: %s", yn(r > 0 && atoi_(b) > 0));
    long c = K(SYS_fork);
    if (c == 0) {
        K(SYS_setresuid, 1000, 1001, 1002);
        slurp("/proc/self/status", b, sizeof(b));
        char *u = strstr(b, "Uid:");
        K(SYS_exit, u && !strncmp(u, "Uid:\t1000\t1001\t1002\t1001", 22) ? 1 : 0);
    }
    int st = 0; K(SYS_wait4, c, &st, 0, 0);
    NOTE("status Uid reflects setresuid: %d", (st >> 8) & 0xff);
    return 0;
}
