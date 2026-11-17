/* init: PID 1. Opens the console, starts the login shell and the MCP daemon,
 * respawns them when they exit and reaps orphaned processes. */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/time.h>
#include <libc/errno.h>
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

int main(void) {
    open_console();
    setenv("PATH", "/bin:/sbin:/usr/bin", 0);
    setenv("HOME", "/root", 0);
    setenv("TERM", "linux", 0);
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
