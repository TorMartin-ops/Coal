/* Kernel behaviour beyond the plain system call ABI: SA_RESTART, pipe capacity and PIPE_BUF,
 * removing open directories, select/pselect6 and socketpair, loopback networking, unreachable UDP ports.
 * Everything runs inside one Coal OS instance, using /tmp only. */

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/stdlib.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>
#include <libc/signal.h>
#include <libc/poll.h>
#include <libc/time.h>
#include <libc/syscall.h>
#include <libc/sys/stat.h>
#include <libc/sys/wait.h>
#include <libc/sys/time.h>
#include <libc/sys/socket.h>
#include "check.h"

#define SA_RESTART  0x10000000
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#define SYS_SOCKETPAIR_NR SYS_SOCKETPAIR

struct ksigaction { void (*handler)(int); unsigned long flags; void *restorer; unsigned mask[2]; };

static void set_handler(int sig, void (*h)(int), unsigned long flags) {
    struct ksigaction sa = { h, flags, 0, { 0, 0 } };       /* no SA_RESTORER: the kernel supplies the return code */
    __sc(SYS_RT_SIGACTION, sig, (long)&sa, 0, 8, 0);
}

static void sigprocmask_set(int how, unsigned mask) {
    unsigned set[2] = { mask, 0 };
    __sc(SYS_RT_SIGPROCMASK, how, (long)set, 0, 8, 0);
}

static volatile int hits;
static void on_signal(int sig) { hits++; }

static void msleep(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* A system call with six arguments (the sixth goes in ebp). */
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    __asm__ volatile("pushl %[f]\n\tpushl %%ebp\n\tmovl 4(%%esp), %%ebp\n\tint $0x80\n\tpopl %%ebp\n\taddl $4, %%esp"
                     : "=a"(r)
                     : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), [f] "m"(f)
                     : "memory", "cc");
    return r;
}

static long now_ms(void) {
    struct timespec ts;
    __sc(SYS_CLOCK_GETTIME, 1, (long)&ts, 0, 0, 0);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A child that sends sig to its parent after delay_ms and then exits after tail_ms. */
static pid_t signaller(int sig, long delay_ms, long tail_ms) {
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        msleep(delay_ms);
        kill(parent, sig);
        msleep(tail_ms);
        _exit(0);
    }
    return pid;
}

//============================================================================
// SA_RESTART
//============================================================================

static void test_sa_restart(void) {
    int fds[2];
    char c;

    /* Without SA_RESTART a blocked read fails with EINTR. */
    CHECK(pipe(fds) == 0);
    set_handler(SIGUSR1, on_signal, 0);
    hits = 0;
    pid_t kid = signaller(SIGUSR1, 100, 0);
    CHECK(read(fds[0], &c, 1) < 0 && errno == EINTR && hits == 1);
    waitpid(kid, NULL, 0);

    /* With SA_RESTART the read goes on waiting and returns the byte written later. */
    set_handler(SIGUSR1, on_signal, SA_RESTART);
    hits = 0;
    pid_t parent = getpid();
    kid = fork();
    if (kid == 0) {
        msleep(100);
        kill(parent, SIGUSR1);
        msleep(100);
        write(fds[1], "x", 1);
        _exit(0);
    }
    CHECK(read(fds[0], &c, 1) == 1 && c == 'x' && hits == 1);
    waitpid(kid, NULL, 0);

    /* write to a full pipe restarts too: fill it, let a child signal and then drain it. */
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    char block[4096];
    memset(block, 'w', sizeof block);
    while (write(fds[1], block, sizeof block) > 0) { }
    fcntl(fds[1], F_SETFL, 0);
    hits = 0;
    kid = fork();
    if (kid == 0) {
        msleep(100);
        kill(parent, SIGUSR1);
        msleep(100);
        char sink[4096];
        read(fds[0], sink, sizeof sink);
        _exit(0);
    }
    CHECK(write(fds[1], "y", 1) == 1 && hits == 1);
    waitpid(kid, NULL, 0);
    close(fds[0]);
    close(fds[1]);

    /* wait4 restarts: the child outlives the signal. */
    hits = 0;
    kid = signaller(SIGUSR1, 100, 200);
    int status = 0;
    CHECK(waitpid(kid, &status, 0) == kid && WIFEXITED(status) && hits == 1);

    /* poll, nanosleep and select never restart, whatever SA_RESTART says. */
    CHECK(pipe(fds) == 0);
    hits = 0;
    kid = signaller(SIGUSR1, 100, 300);
    struct pollfd pfd = { fds[0], POLLIN, 0 };
    CHECK(poll(&pfd, 1, 2000) < 0 && errno == EINTR && hits == 1);
    waitpid(kid, NULL, 0);

    hits = 0;
    kid = signaller(SIGUSR1, 100, 300);
    struct timespec ts = { 2, 0 };
    CHECK(nanosleep(&ts, NULL) < 0 && errno == EINTR && hits == 1);
    waitpid(kid, NULL, 0);

    hits = 0;
    kid = signaller(SIGUSR1, 100, 300);
    unsigned rset = 1u << fds[0];
    CHECK(__sc(SYS_NEWSELECT, fds[0] + 1, (long)&rset, 0, 0, 0) == -EINTR && hits == 1);
    waitpid(kid, NULL, 0);
    close(fds[0]);
    close(fds[1]);
    set_handler(SIGUSR1, SIG_DFL, 0);
}

//============================================================================
// Pipes: 64 KiB capacity, PIPE_BUF atomicity
//============================================================================

static void test_pipe_capacity(void) {
    int fds[2];
    static char block[4096], big[100000];

    CHECK(pipe(fds) == 0);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);             /* F_SETFL must reach the pipe itself */
    long total = 0, r;
    memset(block, 'p', sizeof block);
    while ((r = write(fds[1], block, sizeof block)) > 0) total += r;
    CHECK(total == 65536 && r < 0 && errno == EAGAIN);

    /* Free 100 bytes: a 200 byte write (<= PIPE_BUF) is all or nothing, a 100 byte one fits. */
    char sink[100];
    CHECK(read(fds[0], sink, 100) == 100);
    CHECK(write(fds[1], block, 200) < 0 && errno == EAGAIN);
    CHECK(write(fds[1], block, 100) == 100);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    while (read(fds[0], big, sizeof big) > 0) { }
    CHECK(read(fds[0], big, 1) < 0 && errno == EAGAIN);

    /* A big write to an empty pipe is partial when non-blocking: it fills the pipe. */
    memset(big, 'q', sizeof big);
    CHECK(write(fds[1], big, sizeof big) == 65536);
    close(fds[0]);
    close(fds[1]);

    /* Two writers with 512 byte records: the stream must stay record aligned. */
    CHECK(pipe(fds) == 0);
    for (int w = 0; w < 2; w++) {
        if (fork() == 0) {
            char rec[512];
            memset(rec, 'A' + w, sizeof rec);
            for (int i = 0; i < 300; i++) write(fds[1], rec, sizeof rec);
            _exit(0);
        }
    }
    close(fds[1]);
    static char stream[2 * 300 * 512];
    long got = 0;
    while ((r = read(fds[0], stream + got, sizeof stream - got)) > 0) got += r;
    CHECK(got == (long)sizeof stream);
    int torn = 0;
    for (long rec = 0; rec < got / 512; rec++)
        for (int i = 1; i < 512; i++) torn += stream[rec * 512 + i] != stream[rec * 512];
    CHECK(torn == 0);
    while (waitpid(-1, NULL, 0) > 0) { }
    close(fds[0]);
}

//============================================================================
// Removing open directories
//============================================================================

static long dir_entries(int fd) {
    char buf[512];
    long n = __sc(SYS_GETDENTS64, fd, (long)buf, sizeof buf, 0, 0), count = 0;

    for (long off = 0; n > 0 && off < n;) {
        unsigned short reclen = *(unsigned short *)(buf + off + 16);
        const char *name = buf + off + 19;
        count += strcmp(name, ".") && strcmp(name, "..");
        off += reclen;
    }
    return n < 0 ? n : count;
}

static void test_open_directory(void) {
    struct stat st;

    CHECK(mkdir("/tmp/k6dir", 0755) == 0);
    int fd = open("/tmp/k6dir", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    CHECK(rmdir("/tmp/k6dir") == 0);                /* the open directory does not stop it */
    CHECK(stat("/tmp/k6dir", &st) < 0 && errno == ENOENT);
    CHECK(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink == 0);
    CHECK(dir_entries(fd) == 0);                    /* it reads as empty */
    CHECK(mkdir("/tmp/k6dir", 0755) == 0);          /* the name is free again meanwhile */
    CHECK(close(fd) == 0);
    CHECK(rmdir("/tmp/k6dir") == 0);

    /* rename over an empty directory that is open */
    CHECK(mkdir("/tmp/k6a", 0755) == 0 && mkdir("/tmp/k6b", 0755) == 0);
    int f = open("/tmp/k6a/file", O_CREAT | O_WRONLY, 0644);
    CHECK(f >= 0 && close(f) == 0);
    fd = open("/tmp/k6b", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    CHECK(rename("/tmp/k6a", "/tmp/k6b") == 0);
    CHECK(dir_entries(fd) == 0);
    CHECK(access("/tmp/k6b/file", F_OK) == 0 && access("/tmp/k6a", F_OK) < 0);
    CHECK(close(fd) == 0);
    CHECK(unlink("/tmp/k6b/file") == 0 && rmdir("/tmp/k6b") == 0);

    /* a directory that is not empty still refuses */
    CHECK(mkdir("/tmp/k6c", 0755) == 0);
    f = open("/tmp/k6c/x", O_CREAT | O_WRONLY, 0644);
    close(f);
    fd = open("/tmp/k6c", O_RDONLY | O_DIRECTORY);
    CHECK(rmdir("/tmp/k6c") < 0 && errno == ENOTEMPTY);
    close(fd);
    unlink("/tmp/k6c/x");
    rmdir("/tmp/k6c");
}

//============================================================================
// select, pselect6, socketpair
//============================================================================

struct tv { long sec, usec; };
struct ts { long sec, nsec; };

static long select_one(int fd, int write_set, struct tv *tv) {
    unsigned set = 1u << fd;
    long r = __sc(SYS_NEWSELECT, fd + 1, write_set ? 0 : (long)&set, write_set ? (long)&set : 0, 0, (long)tv);
    return r > 0 && !(set & (1u << fd)) ? -1000 : r;
}

static void test_select(void) {
    int fds[2];
    char c;
    struct tv tv;

    CHECK(pipe(fds) == 0);
    tv = (struct tv){ 0, 0 };
    CHECK(select_one(fds[0], 0, &tv) == 0);     /* empty: nothing readable */
    CHECK(select_one(fds[1], 1, &tv) == 1);      /* room: writable */
    CHECK(write(fds[1], "z", 1) == 1);
    CHECK(select_one(fds[0], 0, &tv) == 1);

    CHECK(read(fds[0], &c, 1) == 1);
    long t0 = now_ms();
    tv = (struct tv){ 0, 200000 };
    CHECK(select_one(fds[0], 0, &tv) == 0);     /* times out after ~200 ms and reports the time left */
    long waited = now_ms() - t0;
    CHECK(waited >= 190 && waited < 400);
    CHECK(tv.sec == 0 && tv.usec < 50000);

    /* a descriptor that is not open is EBADF; unused sets are skipped */
    unsigned bad = 1u << 20;
    CHECK(__sc(SYS_NEWSELECT, 21, (long)&bad, 0, 0, (long)&tv) == -EBADF);
    CHECK(__sc(SYS_NEWSELECT, 2000, 0, 0, 0, 0) == -EINVAL);

    /* blocks without a timeout until a child writes */
    pid_t kid = fork();
    if (kid == 0) { msleep(100); write(fds[1], "k", 1); _exit(0); }
    unsigned set = 1u << fds[0];
    CHECK(__sc(SYS_NEWSELECT, fds[0] + 1, (long)&set, 0, 0, 0) == 1 && set == (1u << fds[0]));
    waitpid(kid, NULL, 0);
    read(fds[0], &c, 1);

    /* the old select (82) takes one pointer to its five arguments */
    long args[5] = { fds[0] + 1, (long)&set, 0, 0, 0 };
    tv = (struct tv){ 0, 0 };
    args[4] = (long)&tv;
    set = 1u << fds[0];
    CHECK(__sc(SYS_SELECT, (long)args, 0, 0, 0, 0) == 0);

    /* pselect6 with a nanosecond timeout and no mask */
    struct ts tsp = { 0, 100 * 1000000 };
    set = 1u << fds[0];
    t0 = now_ms();
    CHECK(sc6(SYS_PSELECT6, fds[0] + 1, (long)&set, 0, 0, (long)&tsp, 0) == 0);
    CHECK(now_ms() - t0 >= 90);

    /* pselect6 unblocks a signal atomically: it returns EINTR with the handler run, and the old mask is back */
    unsigned empty[2] = { 0, 0 }, cur[2];
    long sigarg[2] = { (long)empty, 8 };
    struct ts five = { 5, 0 }, brief = { 0, 300 * 1000000 };
    unsigned usr1 = 1u << (SIGUSR1 - 1);

    set_handler(SIGUSR1, on_signal, 0);
    sigprocmask_set(0 /* SIG_BLOCK */, usr1);
    hits = 0;
    kid = signaller(SIGUSR1, 100, 0);
    set = 1u << fds[0];
    t0 = now_ms();
    CHECK(sc6(SYS_PSELECT6, fds[0] + 1, (long)&set, 0, 0, (long)&five, (long)sigarg) == -EINTR);
    CHECK(hits == 1 && now_ms() - t0 < 2000);
    waitpid(kid, NULL, 0);
    __sc(SYS_RT_SIGPROCMASK, 0, 0, (long)cur, 8, 0);
    CHECK(cur[0] & usr1);                           /* SIGUSR1 is blocked again */

    hits = 0;                                       /* without a mask the signal waits */
    kid = signaller(SIGUSR1, 50, 0);
    set = 1u << fds[0];
    CHECK(sc6(SYS_PSELECT6, fds[0] + 1, (long)&set, 0, 0, (long)&brief, 0) == 0 && hits == 0);
    waitpid(kid, NULL, 0);
    sigprocmask_set(1 /* SIG_UNBLOCK */, usr1);
    CHECK(hits == 1);
    set_handler(SIGUSR1, SIG_DFL, 0);
    close(fds[0]);
    close(fds[1]);
}

static void test_socketpair(void) {
    int sv[2];
    char buf[16];

    CHECK(__sc(SYS_SOCKETPAIR_NR, 1, SOCK_STREAM, 0, (long)sv, 0) == 0);
    struct stat st;
    CHECK(fstat(sv[0], &st) == 0 && (st.st_mode & S_IFMT) == 0140000);
    CHECK(write(sv[0], "ping", 4) == 4 && read(sv[1], buf, sizeof buf) == 4 && !memcmp(buf, "ping", 4));
    CHECK(write(sv[1], "pong", 4) == 4 && read(sv[0], buf, sizeof buf) == 4 && !memcmp(buf, "pong", 4));
    CHECK(send(sv[0], "abc", 3, 0) == 3 && recv(sv[1], buf, sizeof buf, 0) == 3);   /* socket calls work on it */

    unsigned set = 1u << sv[0];
    struct tv tv = { 0, 0 };
    CHECK(__sc(SYS_NEWSELECT, sv[0] + 1, (long)&set, 0, 0, (long)&tv) == 0);       /* nothing to read yet */
    CHECK(write(sv[1], "q", 1) == 1);
    set = 1u << sv[0];
    CHECK(__sc(SYS_NEWSELECT, sv[0] + 1, (long)&set, 0, 0, (long)&tv) == 1);
    CHECK(read(sv[0], buf, 1) == 1);

    /* shutdown(SHUT_WR) is the peer's end of file, but the other direction keeps working */
    CHECK(shutdown(sv[0], SHUT_WR) == 0);
    CHECK(read(sv[1], buf, sizeof buf) == 0);
    CHECK(write(sv[1], "back", 4) == 4 && read(sv[0], buf, sizeof buf) == 4);

    /* closing one end: the other reads EOF and writes get EPIPE */
    signal(SIGPIPE, SIG_IGN);
    CHECK(close(sv[0]) == 0);
    CHECK(read(sv[1], buf, sizeof buf) == 0);
    CHECK(write(sv[1], "x", 1) < 0 && errno == EPIPE);
    close(sv[1]);
    signal(SIGPIPE, SIG_DFL);

    /* what is not supported says so */
    CHECK(__sc(SYS_SOCKETPAIR_NR, 1, SOCK_DGRAM, 0, (long)sv, 0) == -ESOCKTNOSUPPORT);
    CHECK(__sc(SYS_SOCKETPAIR_NR, 2, SOCK_STREAM, 0, (long)sv, 0) == -EAFNOSUPPORT);

    /* a socketpair crosses fork: the child answers through its end */
    CHECK(__sc(SYS_SOCKETPAIR_NR, 1, SOCK_STREAM, 0, (long)sv, 0) == 0);
    pid_t kid = fork();
    if (kid == 0) {
        char b[8];
        long n = read(sv[1], b, sizeof b);
        for (long i = 0; i < n; i++) b[i] -= 32;
        write(sv[1], b, n);
        _exit(0);
    }
    CHECK(write(sv[0], "abc", 3) == 3 && read(sv[0], buf, sizeof buf) == 3 && !memcmp(buf, "ABC", 3));
    waitpid(kid, NULL, 0);
    close(sv[0]);
    close(sv[1]);
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "restart", test_sa_restart }, { "pipe", test_pipe_capacity }, { "dir", test_open_directory },
        { "select", test_select }, { "socketpair", test_socketpair },
    };

    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++)
        if (argc < 2 || !strcmp(argv[1], tests[i].name)) tests[i].fn();
    return check_report("kernel_test");
}
