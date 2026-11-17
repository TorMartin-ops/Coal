/* init: PID 1. Opens the console, starts the login shell and the MCP daemon,
 * respawns them when they exit and reaps orphaned processes. */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/time.h>
#include <libc/errno.h>
#include <libc/syscall.h>
#include <libc/sys/wait.h>

struct service {
    const char *path;
    char *argv[2];
    int console;        /* needs the console as controlling terminal */
    int optional;       /* only started when the file exists */
    pid_t pid;
    time_t started;
};

static struct service services[] = {
    { "/bin/sh", { "-sh", NULL }, 1, 0, 0, 0 },
    { "/sbin/agentd", { "agentd", NULL }, 0, 1, 0, 0 },
};

#define NSERVICES (int)(sizeof services / sizeof services[0])

static void open_console(void) {
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) return;
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
}

static void start(struct service *s) {
    if (s->optional && access(s->path, X_OK) < 0) return;
    /* a service that dies right after it started must not spin the CPU */
    if (s->started && time(NULL) - s->started < 2) sleep(1);
    s->started = time(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: fork: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        if (s->console) open_console();
        execve(s->path, s->argv, environ);
        fprintf(stderr, "init: cannot run %s: %s\n", s->path, strerror(errno));
        _exit(127);
    }
    s->pid = pid;
}

/* Test mode: "coal.test=PROG,ARG,..." on the kernel command line runs that one program with stdout and stderr
 * on COM1, prints its result and powers off. The run needs no input from the host, so it is repeatable. */
static int run_test(char *spec) {
    char *argv[16];
    int argc = 0, status = -1;

    for (char *p = spec; argc < 15; p++) {
        argv[argc++] = p;
        while (*p && *p != ',') p++;
        if (!*p) break;
        *p = '\0';
    }
    argv[argc] = NULL;
    int serial = open("/dev/ttyS0", O_RDWR), null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(serial, 1);
    dup2(serial, 2);
    pid_t pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, environ);
        fprintf(stderr, "init: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &status, 0);
    printf("\n@@COALTEST exit=%d signal=%d\n", (status & 0x7f) ? -1 : (status >> 8) & 0xff, status & 0x7f);
    fflush(stdout);
    sync();
    __syscall_ret(__sc(SYS_REBOOT, 0xfee1dead, 672274793, 0x4321fedcu, 0, 0));
    return 1;
}

static char *cmdline_test(char *buf, size_t cap) {
    int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, cap - 1);
    char *w;

    if (fd >= 0) close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    w = strstr(buf, "coal.test=");
    if (!w) return NULL;
    w += 10;
    for (char *e = w; ; e++)
        if (!*e || *e == ' ' || *e == '\n') { *e = '\0'; break; }
    return w;
}

int main(void) {
    char line[256], *test = cmdline_test(line, sizeof line);

    setenv("PATH", "/bin:/sbin:/usr/bin", 0);
    setenv("HOME", "/root", 0);
    setenv("TERM", "linux", 0);
    if (test) return run_test(test);
    open_console();
    puts("Coal OS init");
    for (;;) {
        for (int i = 0; i < NSERVICES; i++)
            if (!services[i].pid) start(&services[i]);
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            sleep(1);
            continue;
        }
        for (int i = 0; i < NSERVICES; i++)
            if (services[i].pid == pid) services[i].pid = 0;
        /* any other pid was an orphan adopted by init; waiting reaped it */
    }
}
