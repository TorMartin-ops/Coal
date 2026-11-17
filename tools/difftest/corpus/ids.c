/* Identity and system information: uid/gid, groups, uname, sysinfo, /proc, /dev, umask, ttys. */
#include "trace.h"
#include <sys/utsname.h>
#include <sys/sysinfo.h>

int main(void) {
    init();
    R("getuid", K(SYS_getuid));
    R("geteuid", K(SYS_geteuid));
    R("getgid", K(SYS_getgid));
    R("getegid", K(SYS_getegid));
    unsigned g[8];
    R("getgroups(0 size)", POS(K(SYS_getgroups, 0, 0)) >= 0 ? 0 : -1);
    R("getgroups(bad ptr)", K(SYS_getgroups, 8, 8) == -EFAULT ? -EFAULT : 0);
    R("setgroups(0)", K(SYS_setgroups, 0, g));
    R("setuid(0)", K(SYS_setuid, 0));
    R("setresuid(0,0,0)", K(SYS_setresuid, 0, 0, 0));
    R("setresgid(0,0,0)", K(SYS_setresgid, 0, 0, 0));
    R("setreuid(-1,-1)", K(SYS_setreuid, -1, -1));
    long c = K(SYS_fork);
    if (c == 0) {
        long a = K(SYS_setuid, 1000), b = K(SYS_getuid), e = K(SYS_setuid, 0);
        K(SYS_exit, (a == 0 ? 1 : 0) | (b == 1000 ? 2 : 0) | (e == -EPERM ? 4 : 0));
    }
    int st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("child drops root and cannot regain: bits 1 2 4", (st >> 8) & 0xff);

    struct utsname u;
    R("uname", K(SYS_uname, &u));
    NOTE("sysname=%s machine=%s release non-empty=%s version non-empty=%s", u.sysname, u.machine, yn(u.release[0]), yn(u.version[0]));
    char hn[65];
    R("gethostname via uname nodename non-empty", u.nodename[0] ? 0 : -1);
    (void)hn;
    struct sysinfo si;
    R("sysinfo", K(SYS_sysinfo, &si));
    NOTE("uptime>0=%s totalram>0=%s freeram<=totalram=%s procs>0=%s mem_unit>=1=%s", yn(si.uptime >= 0), yn(si.totalram > 0), yn(si.freeram <= si.totalram), yn(si.procs > 0), yn(si.mem_unit >= 1));
    R("umask(0)", K(SYS_umask, 0));
    R("umask(022) returns 0", K(SYS_umask, 022));
    char b[512];
    long f = R("open(/proc/self/status)", K(SYS_open, "/proc/self/status", O_RDONLY));
    long n = K(SYS_read, f, b, sizeof b - 1);
    R("read(/proc/self/status)", POS(n));
    if (n > 0) { b[n] = 0; NOTE("has Name line: %s Pid line: %s", yn(strstr(b, "Name:") != 0), yn(strstr(b, "Pid:") != 0)); }
    K(SYS_close, f);
    f = R("open(/proc/meminfo)", K(SYS_open, "/proc/meminfo", O_RDONLY));
    n = K(SYS_read, f, b, sizeof b - 1);
    R("read(/proc/meminfo)", POS(n));
    if (n > 0) { b[n] = 0; NOTE("has MemTotal: %s MemFree: %s", yn(strstr(b, "MemTotal:") != 0), yn(strstr(b, "MemFree:") != 0)); }
    K(SYS_close, f);
    f = R("open(/proc/uptime)", K(SYS_open, "/proc/uptime", O_RDONLY));
    R("read(/proc/uptime)", POS(K(SYS_read, f, b, sizeof b)));
    K(SYS_close, f);
    R("readlink(/proc/self/exe)", POS(K(SYS_readlink, "/proc/self/exe", b, sizeof b)));
    R("readlink(/proc/self/cwd)", POS(K(SYS_readlink, "/proc/self/cwd", b, sizeof b)));
    RFD("open(/proc/self/fd)", K(SYS_open, "/proc/self/fd", O_RDONLY | O_DIRECTORY));
    RFD("open(/proc/cpuinfo)", K(SYS_open, "/proc/cpuinfo", O_RDONLY));
    RFD("open(/proc/self/maps)", K(SYS_open, "/proc/self/maps", O_RDONLY));
    RFD("open(/proc/mounts)", K(SYS_open, "/proc/mounts", O_RDONLY));
    RFD("open(/proc/version)", K(SYS_open, "/proc/version", O_RDONLY));
    RFD("stat(/dev/console)", K(SYS_open, "/dev/console", O_RDWR));
    STATT("stat(/dev)", "/dev");
    STATT("stat(/proc)", "/proc");
    STATT("stat(/tmp)", "/tmp");
    STATT("stat(/etc/passwd type)", "/etc/passwd");
    R("access(/bin/sh X_OK)", K(SYS_access, "/bin/sh", X_OK));
    RFD("open(/etc/passwd)", K(SYS_open, "/etc/passwd", O_RDONLY));
    R("ioctl(stdin TIOCGWINSZ)", K(SYS_ioctl, 0, 0x5413, b));
    R("isatty-style ioctl(0, TCGETS)", K(SYS_ioctl, 0, 0x5401, b));
    R("setsid(not group leader)", K(SYS_getpid) == K(SYS_getpgid, 0) ? -EPERM : 0);
    R("sched_getscheduler", K(SYS_sched_getscheduler, 0));
    R("sched_getparam", K(SYS_sched_getparam, 0, b));
    R("sched_get_priority_max(OTHER)", K(SYS_sched_get_priority_max, 0));
    R("getcpu", K(SYS_getcpu, b, b + 8, 0));
    R("prctl(PR_SET_PDEATHSIG)", K(SYS_prctl, 1, 9, 0, 0, 0));
    R("prctl(PR_GET_NAME)", K(SYS_prctl, 16, b, 0, 0, 0));
    R("arch_prctl(ARCH_GET_FS)", K(SYS_arch_prctl, 0x1003, b));
    R("arch_prctl(bad code)", K(SYS_arch_prctl, 0x9999, b));
    return 0;
}
