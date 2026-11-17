/* System applets: ps kill uname clear sleep env true false free uptime date
 * which mount umount dmesg reboot halt poweroff */

#include "cb.h"
#include "applets.h"
#include <libc/dirent.h>
#include <libc/signal.h>
#include <libc/time.h>
#include <libc/syscall.h>
#include <libc/sys/utsname.h>
#include <libc/termios.h>
#include <libc/poll.h>

int true_main(int argc, char **argv)  { return 0; }
int false_main(int argc, char **argv) { return 1; }

int clear_main(int argc, char **argv) {
    fputs("\033[H\033[2J", stdout);
    return 0;
}

/* Returns the value of "Key:" in a /proc status-style buffer, or NULL. */
static char *field(char *buf, const char *key) {
    size_t kl = strlen(key);
    for (char *p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == ':') {
            p += kl + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
    }
    return NULL;
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int ps_main(int argc, char **argv) {
    int n, rc = 0;
    char **v = cb_list_dir("/proc", &n);
    if (!v) return cb_err(NULL, "/proc");
    int *pids = malloc((size_t)(n + 1) * sizeof *pids), np = 0;
    for (int i = 0; i < n; i++)
        if (isdigit(v[i][0])) pids[np++] = atoi(v[i]);
    cb_free_names(v, n);
    qsort(pids, (size_t)np, sizeof *pids, cmp_int);
    printf("  PID  PPID STAT COMMAND\n");
    for (int i = 0; i < np; i++) {
        char path[32], buf[512];
        snprintf(path, sizeof path, "/proc/%d/status", pids[i]);
        if (cb_slurp(path, buf, sizeof buf) < 0) continue;      /* exited meanwhile */
        char *name = field(buf, "Name"), *state = field(buf, "State"), *ppid = field(buf, "PPid");
        char *nl = name ? strchr(name, '\n') : NULL;
        if (nl) *nl = 0;
        printf("%5d %5d %4c %s\n", pids[i], ppid ? atoi(ppid) : 0, state ? *state : '?', name ? name : "?");
    }
    free(pids);
    return rc;
}

static const struct { const char *name; int num; } signals[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL },
    { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
    { "TERM", SIGTERM }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
    { "TSTP", SIGTSTP },
};

static int signal_number(const char *s) {
    if (isdigit(*s)) return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; i++)
        if (!strcmp(s, signals[i].name)) return signals[i].num;
    return -1;
}

int kill_main(int argc, char **argv) {
    int sig = SIGTERM, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-l")) {
        for (unsigned k = 0; k < sizeof signals / sizeof signals[0]; k++)
            printf("%2d) SIG%s\n", signals[k].num, signals[k].name);
        return 0;
    }
    if (i + 1 < argc && !strcmp(argv[i], "-s")) {
        sig = signal_number(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        sig = signal_number(argv[i++] + 1);
    }
    if (sig < 0) {
        fprintf(stderr, "kill: invalid signal\n");
        return 1;
    }
    if (i >= argc) return cb_usage("[-SIGNAL] PID...");
    for (; i < argc; i++) {
        char *end;
        long pid = strtol(argv[i], &end, 10);
        if (*end || kill((pid_t)pid, sig) < 0) {
            fprintf(stderr, "kill: (%s) - %s\n", argv[i], *end ? "arguments must be process IDs" : strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

int uname_main(int argc, char **argv) {
    struct utsname u;
    unsigned want = 0;
    int c;
    while ((c = getopt(argc, argv, "asnrvm")) != -1) {
        const char *p = strchr("snrvm", c);
        if (c == 'a') want = 31;
        else if (p) want |= 1u << (p - "snrvm");
        else return 1;
    }
    if (!want) want = 1;
    if (uname(&u) < 0) return cb_err(NULL, "uname");
    const char *f[] = { u.sysname, u.nodename, u.release, u.version, u.machine };
    for (int i = 0, first = 1; i < 5; i++) {
        if (!(want & (1u << i))) continue;
        printf("%s%s", first ? "" : " ", f[i]);
        first = 0;
    }
    putchar('\n');
    return 0;
}

int sleep_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "sleep: missing operand\n");
        return 1;
    }
    /* seconds with an optional fraction: "2", "0.5" */
    const char *s = argv[1];
    char *end;
    struct timespec ts = { strtol(s, &end, 10), 0 };
    if (*end == '.') {
        long scale = 100000000;
        for (end++; isdigit(*end) && scale; end++, scale /= 10) ts.tv_nsec += (*end - '0') * scale;
        while (isdigit(*end)) end++;
    }
    if (*end || end == s) {
        fprintf(stderr, "sleep: invalid time interval '%s'\n", s);
        return 1;
    }
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
    return 0;
}

int nice_main(int argc, char **argv) {
    int inc = 10, i = 1;

    if (argc == 1) {
        printf("%d\n", nice(0));
        return 0;
    }
    if (!strcmp(argv[1], "-n") && argc > 2) {
        inc = atoi(argv[2]);
        i = 3;
    }
    if (i == argc) {
        fprintf(stderr, "usage: nice [-n adjustment] command [arg...]\n");
        return 1;
    }
    errno = 0;
    if (nice(inc) == -1 && errno) fprintf(stderr, "nice: %s\n", strerror(errno));
    execvp(argv[i], argv + i);
    fprintf(stderr, "nice: '%s': %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

int env_main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++) {
        char *eq = strchr(argv[i], '=');
        *eq = 0;
        setenv(argv[i], eq + 1, 1);
    }
    if (i == argc) {
        for (char **e = environ; e && *e; e++) puts(*e);
        return 0;
    }
    fflush(stdout);
    execvp(argv[i], argv + i);
    fprintf(stderr, "env: '%s': %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

static unsigned long meminfo(const char *buf, const char *key, unsigned long dflt) {
    char *p = field((char *)buf, key);
    return p ? strtoul(p, NULL, 10) : dflt;
}

int free_main(int argc, char **argv) {
    char buf[1024];
    int c, shift = 0;                          /* output unit: kB by default */
    while ((c = getopt(argc, argv, "bkm")) != -1) {
        if (c == 'b') shift = -10;
        else if (c == 'k') shift = 0;
        else if (c == 'm') shift = 10;
        else return 1;
    }
    if (cb_slurp("/proc/meminfo", buf, sizeof buf) < 0) return cb_err(NULL, "/proc/meminfo");
    unsigned long total = meminfo(buf, "MemTotal", 0), fr = meminfo(buf, "MemFree", 0);
    unsigned long cache = meminfo(buf, "Buffers", 0) + meminfo(buf, "Cached", 0);
    unsigned long avail = meminfo(buf, "MemAvailable", fr + cache), shared = meminfo(buf, "Shmem", 0);
    unsigned long used = total - fr - cache, v[6] = { total, used, fr, shared, cache, avail };
    printf("%14s%12s%12s%12s%12s%12s\nMem:", "total", "used", "free", "shared", "buff/cache", "available");
    for (int i = 0; i < 6; i++) printf("%12lu", shift < 0 ? v[i] << 10 : v[i] >> shift);
    putchar('\n');
    return 0;
}

int uptime_main(int argc, char **argv) {
    char buf[64];
    if (cb_slurp("/proc/uptime", buf, sizeof buf) < 0) return cb_err(NULL, "/proc/uptime");
    unsigned long up = strtoul(buf, NULL, 10);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    printf(" %02d:%02d:%02d up ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (up >= 86400) printf("%lu day%s, ", up / 86400, up / 86400 > 1 ? "s" : "");
    if (up >= 3600) printf("%lu:%02lu\n", up % 86400 / 3600, up % 3600 / 60);
    else printf("%lu min\n", up / 60);
    return 0;
}

int date_main(int argc, char **argv) {
    time_t now = time(NULL);
    struct tm tm;
    char out[128];
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '+') fmt = argv[i] + 1;
    if (!strcmp(fmt, "%s")) {
        printf("%ld\n", (long)now);
        return 0;
    }
    gmtime_r(&now, &tm);
    strftime(out, sizeof out, fmt, &tm);
    puts(out);
    return 0;
}

int which_main(int argc, char **argv) {
    int rc = 0;
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    for (int i = 1; i < argc; i++) {
        int found = 0;
        for (const char *d = path; *d && !found;) {
            const char *e = strchr(d, ':');
            size_t dl = e ? (size_t)(e - d) : strlen(d);
            char dir[128], *full;
            snprintf(dir, sizeof dir, "%.*s", (int)dl, d);
            full = cb_join(dir, argv[i]);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && !access(full, X_OK)) {
                puts(full);
                found = 1;
            }
            free(full);
            d += dl + (e != NULL);
        }
        if (!found) {
#define CHECK(n) if (!strcmp(argv[i], #n)) found = 1;
            APPLET_LIST(CHECK)
            if (found) puts("/bin/coalbox");
            else rc = 1;
        }
    }
    return rc;
}

int mount_main(int argc, char **argv) {
    const char *type = NULL;
    int c;
    while ((c = getopt(argc, argv, "t:")) != -1) {
        if (c == 't') type = optarg;
        else return 1;
    }
    if (argc - optind == 0) {
        char buf[2048];
        if (cb_slurp("/proc/mounts", buf, sizeof buf) < 0) {
            fputs("mount: the mount list is not available on this kernel\n", stderr);
            return 1;
        }
        fputs(buf, stdout);
        return 0;
    }
    if (argc - optind != 2) return cb_usage("[-t TYPE] DEVICE DIR");
    if (__syscall_ret(__sc(SYS_MOUNT, (long)argv[optind], (long)argv[optind + 1], (long)type, 0, 0)) < 0)
        return cb_err("cannot mount", argv[optind]);
    return 0;
}

int umount_main(int argc, char **argv) {
    if (argc != 2) return cb_usage("DIR");
    if (__syscall_ret(__SC3(SYS_UMOUNT2, argv[1], 0, 0)) < 0) return cb_err("cannot unmount", argv[1]);
    return 0;
}

int dmesg_main(int argc, char **argv) {
    enum { SIZE = 16384 };
    char *buf = malloc(SIZE);
    /* syslog action 3: read all messages remaining in the ring buffer */
    long n = buf ? __syscall_ret(__SC3(SYS_SYSLOG, 3, buf, SIZE)) : -1;
    if (n < 0) return cb_err(NULL, "klogctl");
    cb_write_all(1, buf, (size_t)n);
    return 0;
}

/* reboot, halt and poweroff differ only in the command passed to reboot(2). */
int reboot_main(int argc, char **argv) {
    unsigned cmd = !strcmp(prog, "halt") ? 0xcdef0123u : !strcmp(prog, "poweroff") ? 0x4321fedcu : 0x01234567u;
    sync();
    __syscall_ret(__sc(SYS_REBOOT, 0xfee1dead, 672274793, (long)cmd, 0, 0));
    return cb_err(NULL, prog);
}

int halt_main(int argc, char **argv)     { return reboot_main(argc, argv); }
int poweroff_main(int argc, char **argv) { return reboot_main(argc, argv); }

/* ps, refreshed every second, until q or Ctrl-C. */
int top_main(int argc, char **argv) {
    struct termios old, raw;
    int tty = isatty(0) && isatty(1);
    if (tty) {
        tcgetattr(0, &old);
        raw = old;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }
    for (;;) {
        char up[64] = "", mem[256] = "";
        cb_slurp("/proc/uptime", up, sizeof up);
        cb_slurp("/proc/meminfo", mem, sizeof mem);
        if (tty) fputs("\033[H\033[J", stdout);
        up[strcspn(up, "\n")] = 0;
        printf("top - uptime %s s    (q to quit)\n", up);
        ps_main(0, NULL);
        fflush(stdout);
        if (!tty) return 0;
        struct pollfd pf = { 0, POLLIN, 0 };
        char c;
        if (poll(&pf, 1, 1000) > 0 && read(0, &c, 1) == 1 && (c == 'q' || c == 3)) break;
    }
    tcsetattr(0, TCSANOW, &old);
    return 0;
}
