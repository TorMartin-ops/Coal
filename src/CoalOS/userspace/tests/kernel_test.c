/* Kernel behaviour beyond the plain system call ABI: the vDSO (time page), SA_RESTART, pipe capacity and PIPE_BUF,
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
#include <libc/netinet/in.h>
#include <kernel/cpu/vdso_abi.h>
#include "check.h"

#define SA_RESTART  0x10000000
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#define SYS_SOCKETPAIR_NR SYS_SOCKETPAIR

struct ksigaction { void (*handler)(int); unsigned long flags; void (*restorer)(void); unsigned long mask; };
#define SA_RESTORER 0x04000000ul

extern void __sigreturn(void);              /* libc: calls rt_sigreturn */

static void set_handler(int sig, void (*h)(int), unsigned long flags) {
    struct ksigaction sa = { h, flags | SA_RESTORER, __sigreturn, 0 };
    __sc(SYS_RT_SIGACTION, sig, (long)&sa, 0, 8, 0);
}

static void sigprocmask_set(int how, unsigned long mask) {
    __sc(SYS_RT_SIGPROCMASK, how, (long)&mask, 0, 8, 0);
}

static volatile int hits;
static void on_signal(int sig) { hits++; }

static void msleep(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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
    CHECK(__sc(SYS_SELECT, fds[0] + 1, (long)&rset, 0, 0, 0) == -EINTR && hits == 1);
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
    long r = __sc(SYS_SELECT, fd + 1, write_set ? 0 : (long)&set, write_set ? (long)&set : 0, 0, (long)tv);
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
    CHECK(__sc(SYS_SELECT, 21, (long)&bad, 0, 0, (long)&tv) == -EBADF);
    CHECK(__sc(SYS_SELECT, 2000, 0, 0, 0, 0) == -EINVAL);

    /* blocks without a timeout until a child writes */
    pid_t kid = fork();
    if (kid == 0) { msleep(100); write(fds[1], "k", 1); _exit(0); }
    unsigned set = 1u << fds[0];
    CHECK(__sc(SYS_SELECT, fds[0] + 1, (long)&set, 0, 0, 0) == 1 && set == (1u << fds[0]));
    waitpid(kid, NULL, 0);
    read(fds[0], &c, 1);

    /* pselect6 with a nanosecond timeout and no mask */
    struct ts tsp = { 0, 100 * 1000000 };
    set = 1u << fds[0];
    t0 = now_ms();
    CHECK(__sc6(SYS_PSELECT6, fds[0] + 1, (long)&set, 0, 0, (long)&tsp, 0) == 0);
    CHECK(now_ms() - t0 >= 90);

    /* pselect6 unblocks a signal atomically: it returns EINTR with the handler run, and the old mask is back */
    unsigned empty[2] = { 0, 0 }, cur[2];
    long sigarg[2] = { (long)empty, 8 };
    struct ts five = { 5, 0 }, brief = { 0, 300 * 1000000 };
    unsigned long usr1 = 1ul << (SIGUSR1 - 1);

    set_handler(SIGUSR1, on_signal, 0);
    sigprocmask_set(0 /* SIG_BLOCK */, usr1);
    hits = 0;
    kid = signaller(SIGUSR1, 100, 0);
    set = 1u << fds[0];
    t0 = now_ms();
    CHECK(__sc6(SYS_PSELECT6, fds[0] + 1, (long)&set, 0, 0, (long)&five, (long)sigarg) == -EINTR);
    CHECK(hits == 1 && now_ms() - t0 < 2000);
    waitpid(kid, NULL, 0);
    __sc(SYS_RT_SIGPROCMASK, 0, 0, (long)cur, 8, 0);
    CHECK(cur[0] & usr1);                           /* SIGUSR1 is blocked again */

    hits = 0;                                       /* without a mask the signal waits */
    kid = signaller(SIGUSR1, 50, 0);
    set = 1u << fds[0];
    CHECK(__sc6(SYS_PSELECT6, fds[0] + 1, (long)&set, 0, 0, (long)&brief, 0) == 0 && hits == 0);
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
    CHECK(__sc(SYS_SELECT, sv[0] + 1, (long)&set, 0, 0, (long)&tv) == 0);       /* nothing to read yet */
    CHECK(write(sv[1], "q", 1) == 1);
    set = 1u << sv[0];
    CHECK(__sc(SYS_SELECT, sv[0] + 1, (long)&set, 0, 0, (long)&tv) == 1);
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

//============================================================================
// Loopback networking: TCP and UDP over 127.0.0.1, ICMP port unreachable
//============================================================================

static void lo_addr(struct sockaddr_in *a, unsigned port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/* A socket of the given type bound to 127.0.0.1 and a free port, which is stored in *a. */
static int lo_socket(int type, struct sockaddr_in *a) {
    socklen_t n = sizeof *a;
    int fd = socket(AF_INET, type, 0);

    lo_addr(a, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)a, sizeof *a) != 0 || getsockname(fd, (struct sockaddr *)a, &n) != 0) return -1;
    return fd;
}

static void test_loopback(void) {
    struct sockaddr_in a, peer;
    socklen_t n;
    char buf[64];
    int ls = lo_socket(SOCK_STREAM, &a), c, s;

    CHECK(ls >= 0 && listen(ls, 4) == 0);
    CHECK(a.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && a.sin_port != 0);
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == 0);          /* works with or without a NIC */
    n = sizeof peer;
    s = accept(ls, (struct sockaddr *)&peer, &n);
    CHECK(s >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    CHECK(write(c, "hello", 5) == 5 && read(s, buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    CHECK(write(s, "world!", 6) == 6 && read(c, buf, sizeof buf) == 6 && !memcmp(buf, "world!", 6));
    n = sizeof peer;
    CHECK(getpeername(c, (struct sockaddr *)&peer, &n) == 0 && peer.sin_port == a.sin_port);
    CHECK(close(s) == 0 && read(c, buf, sizeof buf) == 0);            /* orderly shutdown reaches the peer */
    close(c);
    close(ls);

    /* A refused connection: nobody listens on that port. */
    lo_addr(&peer, 1);
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&peer, sizeof peer) < 0 && errno == ECONNREFUSED);
    close(c);

    /* UDP between two sockets, with the sender's address reported. */
    struct sockaddr_in ua, ub, from;
    int u1 = lo_socket(SOCK_DGRAM, &ua), u2 = lo_socket(SOCK_DGRAM, &ub);
    CHECK(u1 >= 0 && u2 >= 0);
    CHECK(sendto(u1, "dgram", 5, 0, (struct sockaddr *)&ub, sizeof ub) == 5);
    n = sizeof from;
    CHECK(recvfrom(u2, buf, sizeof buf, 0, (struct sockaddr *)&from, &n) == 5 && !memcmp(buf, "dgram", 5));
    CHECK(from.sin_port == ua.sin_port && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    /* Port unreachable: a connected UDP socket to a closed port gets ECONNREFUSED, once. */
    int uc = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 2, 0 };
    close(u2);                                                          /* now nothing listens on ub */
    CHECK(connect(uc, (struct sockaddr *)&ub, sizeof ub) == 0);
    setsockopt(uc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(send(uc, "x", 1, 0) == 1);
    CHECK(recv(uc, buf, sizeof buf, 0) < 0 && errno == ECONNREFUSED);
    CHECK(recv(uc, buf, sizeof buf, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    CHECK(send(uc, "x", 1, 0) == 1);
    msleep(50);
    struct pollfd pfd = { uc, POLLIN, 0 };
    CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLERR));
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    CHECK(getsockopt(uc, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == ECONNREFUSED);
    close(uc);
    /* An unconnected socket sends to a closed port without any error. */
    CHECK(sendto(u1, "x", 1, 0, (struct sockaddr *)&ub, sizeof ub) == 1);
    msleep(50);
    CHECK(recv(u1, buf, sizeof buf, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    close(u1);

    /* The interface itself. */
    struct { char name[16]; struct sockaddr_in addr; char pad[8]; } ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ifr, 0, sizeof ifr);
    strcpy(ifr.name, "lo");
    CHECK(__sc(SYS_IOCTL, fd, 0x8915, (long)&ifr, 0, 0) == 0 && ifr.addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    close(fd);
}

/* 1 MiB through a loopback connection in both directions at once, verified byte for byte. */
static void test_loopback_stream(void) {
    enum { TOTAL = 1 << 20 };
    struct sockaddr_in a;
    int ls = lo_socket(SOCK_STREAM, &a), c, s;
    static unsigned char blk[8192];
    long sent = 0, got = 0, bad = 0;

    CHECK(ls >= 0 && listen(ls, 1) == 0);
    pid_t kid = fork();
    if (kid == 0) {                                     /* server: echo everything back */
        s = accept(ls, NULL, NULL);
        for (long r; (r = read(s, blk, sizeof blk)) > 0;)
            for (long off = 0; off < r;) { long w = write(s, blk + off, r - off); if (w <= 0) _exit(1); off += w; }
        _exit(0);
    }
    close(ls);
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
    fcntl(c, F_SETFL, O_NONBLOCK);
    while (got < TOTAL) {
        struct pollfd pfd = { c, POLLIN | (sent < TOTAL ? POLLOUT : 0), 0 };
        if (poll(&pfd, 1, 5000) <= 0) break;
        if ((pfd.revents & POLLOUT) && sent < TOTAL) {
            for (int i = 0; i < (int)sizeof blk; i++) blk[i] = (unsigned char)((sent + i) * 7 >> 3);
            long w = write(c, blk, TOTAL - sent < (long)sizeof blk ? TOTAL - sent : (long)sizeof blk);
            if (w > 0) sent += w;                       /* a short write is fine: the pattern depends on the offset only */
        }
        if (pfd.revents & POLLIN) {
            static unsigned char in[8192];
            long r = read(c, in, sizeof in);
            for (long i = 0; i < r; i++) bad += in[i] != (unsigned char)((got + i) * 7 >> 3);
            if (r > 0) got += r;
        }
    }
    CHECK(sent == TOTAL && got == TOTAL && bad == 0);
    close(c);
    int st = 0;
    CHECK(waitpid(kid, &st, 0) == kid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Small writes with and without TCP_NODELAY: with Nagle and delayed ACKs the held bytes still arrive, in order. */
static void test_tcp_small_writes(void) {
    struct sockaddr_in a;
    int ls = lo_socket(SOCK_STREAM, &a), c, s, v = -1, one = 1, zero = 0;
    socklen_t l = sizeof v;
    char in[256];

    CHECK(ls >= 0 && listen(ls, 1) == 0);
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
    s = accept(ls, NULL, NULL);
    CHECK(s >= 0);
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, &l) == 0 && v == 0);
    CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, &l) == 0 && v == 1);
    CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &zero, sizeof zero) == 0);

    for (int round = 0; round < 2; round++) {               /* round 0 with Nagle, round 1 after TCP_NODELAY is set */
        char want[100];
        for (int i = 0; i < 100; i++) { want[i] = (char)('a' + (i + round) % 26); write(c, &want[i], 1); }
        if (round) CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);   /* releases held data at once */
        int got = 0;
        while (got < 100) {
            struct pollfd pfd = { s, POLLIN, 0 };
            if (poll(&pfd, 1, 2000) <= 0) break;
            long r = read(s, in + got, (size_t)(100 - got));
            if (r <= 0) break;
            got += (int)r;
        }
        CHECK(got == 100 && !memcmp(in, want, 100));
    }
    close(c);
    close(s);
    close(ls);
}

//============================================================================
// Scripts, statfs, links
//============================================================================

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    write(fd, text, strlen(text));
    close(fd);
}

static int run_status(const char *path, char *const argv[]) {
    int st = 0;
    pid_t kid = fork();

    if (kid == 0) { execv(path, argv); _exit(errno == ENOEXEC ? 126 : errno == ELOOP ? 125 : 127); }
    return waitpid(kid, &st, 0) == kid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_exec_script(void) {
    char *const plain[] = { "/tmp/k6s", NULL }, *const with_args[] = { "/tmp/k6a", "3", "y", NULL };

    write_file("/tmp/k6s", "#!/bin/sh\nexit 7\n");
    CHECK(run_status("/tmp/k6s", plain) == 7);
    /* The interpreter is this program, which checks its own arguments: the rest of the line is ONE argument. */
    write_file("/tmp/k6a", "#!  /usr/bin/kernel_test   --exec-args -v  \n");
    CHECK(run_status("/tmp/k6a", with_args) == 42);
    write_file("/tmp/k6n", "not a program\n");
    CHECK(run_status("/tmp/k6n", plain) == 126);                    /* ENOEXEC */
    write_file("/tmp/k6l", "#!/tmp/k6l\n");
    CHECK(run_status("/tmp/k6l", plain) == 125);                    /* an interpreter that is itself a loop: ELOOP */
    write_file("/tmp/k6i", "#!/tmp/k6missing\n");
    CHECK(run_status("/tmp/k6i", plain) == 127);                    /* ENOENT for the interpreter */
    unlink("/tmp/k6s"); unlink("/tmp/k6a"); unlink("/tmp/k6n"); unlink("/tmp/k6l"); unlink("/tmp/k6i");
}

static void test_statfs_links(void) {
    struct { long type, bsize; unsigned long blocks, bfree, bavail, files, ffree; int fsid[2]; long namelen, frsize, flags, spare[4]; } sf;
    char path[8];

    memset(&sf, 0xAA, sizeof sf);
    CHECK(__sc(SYS_STATFS, (long)"/", (long)&sf, 0, 0, 0) == 0);
    CHECK(sf.type == 0x4d44 && sf.bsize >= 512 && sf.blocks > 0 && sf.bfree <= sf.blocks && sf.namelen >= 255);
    CHECK(__sc(SYS_STATFS, (long)"/", 0, 0, 0, 0) == -EFAULT);
    CHECK(__sc(SYS_STATFS, (long)"/nonexistent", (long)&sf, 0, 0, 0) == -ENOENT);
    unsigned long free_before = sf.bfree;
    write_file("/tmp/k6big", "x");
    int fd = open("/tmp/k6big", O_WRONLY | O_APPEND);
    char blk[8192];
    memset(blk, 1, sizeof blk);
    CHECK(write(fd, blk, sizeof blk) == sizeof blk);
    memset(&sf, 0, sizeof sf);
    CHECK(__sc(SYS_FSTATFS, fd, (long)&sf, 0, 0, 0) == 0 && sf.bfree < free_before);   /* free space shrank */
    close(fd);
    unlink("/tmp/k6big");
    CHECK(__sc(SYS_STATFS, (long)"/proc", (long)&sf, 0, 0, 0) == 0 && sf.type == 0x9fa0);
    unsigned old[16];
    CHECK(__sc(SYS_STATFS, (long)"/", (long)old, 0, 0, 0) == 0 && old[0] == 0x4d44 && old[2] > 0);

    char mounts[256];
    fd = open("/proc/mounts", O_RDONLY);
    long n = fd < 0 ? 0 : read(fd, mounts, sizeof mounts - 1);
    mounts[n > 0 ? n : 0] = 0;
    if (fd >= 0) close(fd);
    CHECK((strstr(mounts, "/dev/hda / vfat rw 0 0\n") || strstr(mounts, "/dev/vda / vfat rw 0 0\n")) &&
          strstr(mounts, "proc /proc proc "));

    write_file("/tmp/k6f", "data");
    CHECK(__sc(SYS_LINK, (long)"/tmp/k6f", (long)"/tmp/k6g", 0, 0, 0) == -EPERM);
    CHECK(__sc(SYS_SYMLINK, (long)"/tmp/k6f", (long)"/tmp/k6g", 0, 0, 0) == -EPERM);
    CHECK(__sc(SYS_SYMLINK, (long)"x", (long)"/tmp/k6f", 0, 0, 0) == -EEXIST);
    CHECK(__sc(SYS_LINK, (long)"/tmp/none", (long)"/tmp/k6g", 0, 0, 0) == -ENOENT);
    CHECK(__sc(SYS_READLINK, (long)"/tmp/k6f", (long)path, sizeof path, 0, 0) == -EINVAL);   /* exists, not a link */
    CHECK(__sc(SYS_READLINK, (long)"/tmp/none", (long)path, sizeof path, 0, 0) == -ENOENT);
    CHECK(access("/tmp/k6g", F_OK) < 0);
    unlink("/tmp/k6f");
}

//============================================================================
// FPU state belongs to the process: preemption, signal handlers, fork and execve
//============================================================================

static void sig_clobber_fpu(int sig) {
    (void)sig;
    __asm__ volatile("fninit\n\tfld1\n\tfld1\n\tfaddp\n\tfstp %%st(0)" ::: "memory");     /* leaves the FPU empty */
}

/* x87 value kept on the register stack across `rounds` system calls (and whatever preempts them). */
static double fpu_count(double seed, int rounds) {
    double out;

    __asm__ volatile("fldl %0" : : "m"(seed));
    for (int i = 0; i < rounds; i++) {
        __sc(SYS_SCHED_YIELD, 0, 0, 0, 0, 0);
        __asm__ volatile("fld1\n\tfaddp");
        for (volatile int spin = 0; spin < 2000; spin++) { }
    }
    __asm__ volatile("fstpl %0" : "=m"(out));
    return out;
}

static void test_fpu(void) {
    int st = 0;
    pid_t kids[3];
    double seeds[3] = { 1000.0, 2000.0, 3000.0 };

    for (int i = 0; i < 3; i++) {                       /* three processes interleave on the same registers */
        if ((kids[i] = fork()) == 0) _exit(fpu_count(seeds[i], 2000) == seeds[i] + 2000 ? 0 : 1);
    }
    for (int i = 0; i < 3; i++) CHECK(waitpid(kids[i], &st, 0) == kids[i] && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    double v = 7.0, after;                              /* a handler that wrecks the FPU must not change what was interrupted */
    set_handler(SIGUSR1, sig_clobber_fpu, 0);
    __asm__ volatile("fldl %0" : : "m"(v));
    kill(getpid(), SIGUSR1);
    __asm__ volatile("fstpl %0" : "=m"(after));
    CHECK(after == 7.0);
    set_handler(SIGUSR1, SIG_DFL, 0);

    v = 42.0;                                           /* fork copies the registers; each side keeps its own */
    __asm__ volatile("fldl %0" : : "m"(v));
    pid_t kid = fork();
    if (kid == 0) {
        __asm__ volatile("fld1\n\tfaddp\n\tfstpl %0" : "=m"(after));
        _exit(after == 43.0 ? 0 : 1);
    }
    CHECK(waitpid(kid, &st, 0) == kid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    __asm__ volatile("fstpl %0" : "=m"(after));
    CHECK(after == 42.0);

    kid = fork();                                       /* execve starts with a clean FPU */
    if (kid == 0) {
        __asm__ volatile("fldl %0" : : "m"(v));
        execv("/usr/bin/kernel_test", (char *const[]){ "kernel_test", "--fpu-clean", NULL });
        _exit(127);
    }
    CHECK(waitpid(kid, &st, 0) == kid && WIFEXITED(st) && WEXITSTATUS(st) == 42);
}

//============================================================================
// Out of memory: the biggest process is killed, the rest carries on
//============================================================================

static long free_kb(void) {
    char buf[256];
    int fd = open("/proc/meminfo", O_RDONLY);
    long n = fd < 0 ? 0 : read(fd, buf, sizeof buf - 1), kb = 0;

    if (fd >= 0) close(fd);
    buf[n > 0 ? n : 0] = 0;
    const char *p = strstr(buf, "MemFree:");
    for (p = p ? p + 8 : ""; *p == ' '; p++) { }
    while (*p >= '0' && *p <= '9') kb = kb * 10 + *p++ - '0';
    return kb;
}

/* SIGCHLD reaches a parent that handles it, so a poll() sleeper wakes when a child exits. */
static void test_sigchld(void) {
    int fds[2];
    struct pollfd pfd;

    CHECK(pipe(fds) == 0);
    pfd.fd = fds[0]; pfd.events = POLLIN;
    set_handler(SIGCHLD, on_signal, 0);
    hits = 0;
    pid_t kid = fork();
    if (kid == 0) { msleep(100); _exit(3); }
    long t0 = now_ms();
    CHECK(poll(&pfd, 1, 5000) < 0 && errno == EINTR && hits == 1 && now_ms() - t0 < 2000);
    int st = 0;
    CHECK(waitpid(kid, &st, 0) == kid && WEXITSTATUS(st) == 3);
    set_handler(SIGCHLD, (void (*)(int))0, 0);
    close(fds[0]); close(fds[1]);
}

struct krlimit { unsigned long cur, max; };

#define RLIMIT_CPU   0
#define RLIMIT_CORE  4
#define RLIMIT_NPROC 6
#define RLIMIT_AS    9
#define MB           (1ul << 20)

static long set_limit(int res, unsigned long cur, unsigned long max) {
    struct krlimit l = { cur, max };
    return __sc(SYS_SETRLIMIT, res, (long)&l, 0, 0, 0);
}

static long get_limit(int res, struct krlimit *l) { return __sc(SYS_GETRLIMIT, res, (long)l, 0, 0, 0); }

static int count_procs(void) {
    char buf[1024];
    int n = 0, fd = open("/proc", O_RDONLY | O_DIRECTORY);
    for (long r; (r = __sc(SYS_GETDENTS64, fd, (long)buf, sizeof buf, 0, 0)) > 0; )
        for (long off = 0; off < r; off += *(unsigned short *)(buf + off + 16))
            n += buf[off + 19] >= '0' && buf[off + 19] <= '9';
    close(fd);
    return n;
}

/* True if /proc/self/status lists the limit among the RlimitHits. */
static int hit_recorded(const char *what) {
    char buf[256];
    int fd = open("/proc/self/status", O_RDONLY);
    long n = fd < 0 ? 0 : read(fd, buf, sizeof buf - 1);
    if (fd >= 0) close(fd);
    buf[n > 0 ? n : 0] = 0;
    char *h = strstr(buf, "RlimitHits:");
    return h && strstr(h, what) != NULL;
}

static int child_ok(pid_t kid) {
    int st = 0;
    return waitpid(kid, &st, 0) == kid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* setrlimit/ugetrlimit/prlimit64 and the three enforced limits; each case runs in a child so the limits stay put. */
static void test_rlimit(void) {
    struct krlimit l;
    unsigned long long pl[2] = { 0, 0 }, nl[2] = { 50, 60 };

    CHECK(get_limit(RLIMIT_CPU, &l) == 0 && l.cur == ~0ul && l.max == ~0ul);
    CHECK(get_limit(RLIMIT_CORE, &l) == 0 && l.cur == ~0ul);        /* not enforced: reported as unlimited */
    CHECK(get_limit(99, &l) == -EINVAL);
    CHECK(set_limit(RLIMIT_CORE, 0, 0) == -EINVAL);                 /* not enforced: refused, not faked */

    /* cur above max is invalid; raising the hard limit is not permitted; values read back */
    pid_t kid = fork();
    if (kid == 0)
        _exit(set_limit(RLIMIT_AS, 2, 1) == -EINVAL && set_limit(RLIMIT_NPROC, 100, 200) == 0 &&
              set_limit(RLIMIT_NPROC, 100, 300) == -EPERM && get_limit(RLIMIT_NPROC, &l) == 0 &&
              l.cur == 100 && l.max == 200 ? 0 : 1);
    CHECK(child_ok(kid));

    /* prlimit64 sees the same limits with 64-bit values */
    CHECK(__sc(SYS_PRLIMIT64, 0, RLIMIT_NPROC, 0, (long)pl, 0) == 0 && pl[0] == ~0ull && pl[1] == ~0ull);
    kid = fork();
    if (kid == 0) {
        long ok = __sc(SYS_PRLIMIT64, 0, RLIMIT_NPROC, (long)nl, (long)pl, 0) == 0 && pl[0] == ~0ull;
        ok = ok && __sc(SYS_PRLIMIT64, 0, RLIMIT_NPROC, 0, (long)pl, 0) == 0 && pl[0] == 50 && pl[1] == 60;
        _exit(ok ? 0 : 1);
    }
    CHECK(child_ok(kid));

    /* RLIMIT_AS: a mapping or brk growth beyond the limit fails with ENOMEM, small ones still work */
    kid = fork();
    if (kid == 0) {
        long cur = __sc(SYS_BRK, 0, 0, 0, 0, 0);
        set_limit(RLIMIT_AS, 64 * MB, 64 * MB);
        long big = __sc6(SYS_MMAP, 0, 128 * MB, 3, 0x22, -1, 0);
        long small = __sc6(SYS_MMAP, 0, 64 << 10, 3, 0x22, -1, 0);
        long grown = __sc(SYS_BRK, cur + 128 * MB, 0, 0, 0, 0);
        _exit(big == -ENOMEM && small > 0 && grown == cur && hit_recorded("as") ? 0 : 1);
    }
    CHECK(child_ok(kid));

    /* RLIMIT_NPROC: once the system holds that many processes fork fails with EAGAIN */
    kid = fork();
    if (kid == 0) {
        set_limit(RLIMIT_NPROC, count_procs() + 1, ~0ul);
        pid_t a = fork();
        if (a == 0) { msleep(300); _exit(0); }
        pid_t b = fork();
        int blocked = b < 0 && errno == EAGAIN;
        if (b == 0) _exit(0);
        waitpid(a, NULL, 0);
        _exit(a > 0 && blocked && hit_recorded("nproc") ? 0 : 1);
    }
    CHECK(child_ok(kid));

    /* RLIMIT_CPU: SIGXCPU after one second of CPU time; a runaway loop does not outlive it */
    kid = fork();
    if (kid == 0) {
        set_limit(RLIMIT_CPU, 1, 3);
        for (;;) { }
    }
    int st = 0;
    long t0 = now_ms();
    CHECK(waitpid(kid, &st, 0) == kid && WIFSIGNALED(st) && WTERMSIG(st) == SIGXCPU);
    CHECK(now_ms() - t0 < 6000);

    /* a limit hit by a grandchild is visible on the process that waited for it */
    kid = fork();
    if (kid == 0) {
        int before = hit_recorded("as");        /* read before the grandchild can run and exit */
        pid_t g = fork();
        if (g == 0) {
            set_limit(RLIMIT_AS, 64 * MB, 64 * MB);
            __sc6(SYS_MMAP, 0, 128 * MB, 3, 0x22, -1, 0);
            _exit(0);
        }
        waitpid(g, NULL, 0);
        _exit(!before && hit_recorded("as") ? 0 : 1);
    }
    CHECK(child_ok(kid));
}

/* Number of "out of memory" lines in the kernel log. */
static int oom_log_lines(void) {
    static char log[16384];
    long n = __sc(SYS_SYSLOG, 3, (long)log, sizeof log - 1, 0, 0);
    int lines = 0;
    if (n <= 0) return -1;
    log[n] = 0;
    for (const char *p = log; (p = strstr(p, "out of memory: killing")); p++) lines++;
    return lines;
}

/* Two children each fill 60% of the free memory and hold it for a second, so memory runs out. */
static void test_oom_round(void) {
    long before = free_kb(), mb = before / 1024 * 6 / 10;
    int killed = 0, finished = 0, logged = oom_log_lines(), sts[2] = {0, 0};

    CHECK(mb >= 16);
    for (int i = 0; i < 2; i++) {
        if (fork() == 0) {
            volatile char *m = (volatile char *)(long)__sc6(SYS_MMAP, 0, (long)mb << 20, 3, 0x22, -1, 0);
            for (long off = 0; off < mb << 20; off += 4096) m[off] = (char)(off >> 12);
            sleep(1);
            for (long off = 0; off < mb << 20; off += 4096)
                if (m[off] != (char)(off >> 12)) _exit(2);
            _exit(0);
        }
    }
    for (int i = 0; i < 2; i++) {
        int st = 0;
        waitpid(-1, &st, 0);
        sts[i] = st;
        killed += WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
        finished += WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    if (killed != 1 || finished != 1) printf("DBGST %x %x\n", sts[0], sts[1]);
    CHECK(killed == 1 && finished == 1);            /* the biggest user died, the other one kept its data intact */
    CHECK(oom_log_lines() == logged + 1);           /* and the kill was logged once */
    CHECK(free_kb() > before / 10 * 9);             /* and its memory came back */
    CHECK(getpid() > 0 && write(1, "", 0) == 0);    /* the parent never noticed */
}

static void test_oom(void) {
    for (int i = 0; i < 20; i++) test_oom_round();
}

/* Many short processes that exit while the timer preempts them: an exiting process must never be switched back to
 * with its freed page directory loaded (the debug kernel reports a freed directory that is still in CR3). */
static void test_exit_preempted(void) {
    int bad = 0;
    for (int round = 0; round < 40; round++) {
        pid_t kid[8];
        for (int i = 0; i < 8; i++) {
            kid[i] = fork();
            if (kid[i] == 0) {
                if (i & 1) {
                    close(1);
                    execv("/bin/sh", (char *const[]){"sh", "-c", "exit 0", NULL});
                }
                for (volatile int spin = 0; spin < 20000 * (i + 1); spin++) {}
                _exit(i & 1 ? 127 : i);
            }
        }
        for (int i = 0; i < 8; i++) {
            int st = 0;
            bad += waitpid(kid[i], &st, 0) != kid[i] || !WIFEXITED(st) || WEXITSTATUS(st) != (i & 1 ? 0 : i);
        }
    }
    CHECK(bad == 0);
}

/* kill(), getpgid(), getpriority() and prlimit() look processes up while others free zombies: no step onto a freed entry
 * (the debug kernel reports the stale read) */
static void test_kill_walk(void) {
    pid_t reaper[3];
    unsigned long lim[2];
    for (int r = 0; r < 3; r++) {
        reaper[r] = fork();
        if (reaper[r] == 0) {
            for (int i = 0; i < 400; i++) {
                pid_t k = fork();
                if (k == 0) _exit(0);
                waitpid(k, NULL, 0);
            }
            _exit(0);
        }
    }
    long ok = 0;
    int left = 3, bad = 0;
    while (left) {
        for (int i = 0; i < 50; i++) ok += kill(-30000, 0) < 0 && errno == ESRCH;
        for (int i = 0; i < 50; i++) {      /* lookups by pid while the reapers free their children */
            long pid = 2 + (i + ok) % 40;
            __sc(SYS_GETPGID, pid, 0, 0, 0, 0);
            __sc(SYS_GETPRIORITY, 0, pid, 0, 0, 0);
            __sc(SYS_PRLIMIT64, pid, 7, 0, (long)lim, 0);
        }
        int st;
        pid_t w = waitpid(-1, &st, WNOHANG);
        if (w > 0) { left--; bad += !WIFEXITED(st) || WEXITSTATUS(st) != 0; }
    }
    CHECK(ok > 0 && bad == 0);
}

/* poll() on an array that is not 4-byte aligned (the debug kernel reports a misaligned access) */
static void test_poll_unaligned(void) {
    int fds[2];
    char buf[16];
    CHECK(pipe(fds) == 0);
    struct pollfd pf = { fds[1], POLLOUT, 0 };
    memcpy(buf + 2, &pf, sizeof pf);
    CHECK(poll((struct pollfd *)(buf + 2), 1, 0) == 1);
    memcpy(&pf, buf + 2, sizeof pf);
    CHECK(pf.revents == POLLOUT);
    close(fds[0]);
    close(fds[1]);
}

/* --- vDSO: the time page and the TF/signal corner cases --- */

#define AT_SYSINFO_EHDR 33
#define SIGTRAP_NR      5

static unsigned long auxv_value(unsigned long type) {
    char **e = environ;
    while (*e) e++;
    for (unsigned long *a = (unsigned long *)(e + 1); a[0]; a += 2)
        if (a[0] == type) return a[1];
    return 0;
}

static volatile int traps;
static void on_trap(int sig) { traps++; }

static void test_vdso(void) {
    unsigned long ehdr = auxv_value(AT_SYSINFO_EHDR);
    const unsigned char *elf = (const unsigned char *)ehdr;

    CHECK(ehdr && !memcmp(elf, "\177ELF\2", 5));

    long pid = getpid();
    CHECK(pid > 0 && __sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0) == pid);

    /* the sixth argument reaches the kernel: pselect6's sigmask pointer is checked */
    struct timespec zero = { 0, 0 };
    CHECK(__sc6(SYS_PSELECT6, 0, 0, 0, 0, (long)&zero, 0) == 0);
    CHECK(__sc6(SYS_PSELECT6, 0, 0, 0, 0, (long)&zero, 0x10) == -EFAULT);

    /* time: nanosecond resolution, monotonic, and the same clock as the system call */
    struct timespec a, b, c;
    int sub_ms = 0, backwards = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 2000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        sub_ms |= b.tv_nsec % 1000000 != 0;
        backwards |= b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec);
        a = b;
    }
    CHECK(!backwards && (sub_ms || !((const volatile struct vdso_data *)VVAR_BASE)->mult));   /* ticks only without a TSC */
    clock_gettime(CLOCK_MONOTONIC, &a);
    __sc(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&b, 0, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &c);
    long long ta = a.tv_sec * 1000000000LL + a.tv_nsec, tb = b.tv_sec * 1000000000LL + b.tv_nsec;
    long long tc = c.tv_sec * 1000000000LL + c.tv_nsec;
    CHECK(ta <= tb && tb <= tc && tc - ta < 50000000);

    struct timeval tv;
    struct timespec rt;
    time_t t = time(NULL);
    CHECK(gettimeofday(&tv, NULL) == 0 && clock_gettime(CLOCK_REALTIME, &rt) == 0);
    CHECK(t > 1700000000 && tv.tv_sec >= t && tv.tv_sec - t <= 1 && rt.tv_sec >= tv.tv_sec && tv.tv_usec < 1000000);
    CHECK(clock_gettime(7777, &rt) == -1 && errno == EINVAL);          /* another clock goes to the kernel */

    /* a program that single-steps through a system call (TF set) is trapped between the instructions,
     * and the call itself still works: SYSCALL and SYSRET must not trip over TF */
    set_handler(SIGTRAP_NR, on_trap, 0);
    long r;
    traps = 0;
    __asm__ volatile("pushfq\n\torq $0x100, (%%rsp)\n\tpopfq\n\tnop\n\t"
                     "syscall\n\t"
                     "pushfq\n\tandq $~0x100, (%%rsp)\n\tpopfq"
                     : "=a"(r) : "0"((long)SYS_GETPID) : "rcx", "r11", "memory", "cc");
    CHECK(r == pid && traps >= 4);
    set_handler(SIGTRAP_NR, NULL, 0);

    /* NT set in user mode must not reach the kernel's iret (a task return): the signal handler below is
     * entered on the iret path of the call */
    set_handler(SIGUSR1, on_signal, 0);
    hits = 0;
    __asm__ volatile("pushfq\n\torq $0x4000, (%%rsp)\n\tpopfq\n\t"
                     "syscall\n\t"
                     "pushfq\n\tandq $~0x4000, (%%rsp)\n\tpopfq"
                     : "=a"(r) : "0"((long)SYS_KILL), "D"(pid), "S"((long)SIGUSR1) : "rcx", "r11", "memory", "cc");
    CHECK(r == 0 && hits == 1);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--fpu-clean")) {             /* started by test_fpu: the FPU must be freshly initialised */
        unsigned short cw, sw;
        __asm__ volatile("fnstcw %0\n\tfnstsw %1" : "=m"(cw), "=m"(sw));
        return cw == 0x37F && !(sw & 0x3800) ? 42 : 1;
    }
    if (argc == 5 && !strcmp(argv[1], "--exec-args -v"))            /* run as a "#!" interpreter by test_exec_script */
        return !strcmp(argv[2], "/tmp/k6a") && !strcmp(argv[3], "3") && !strcmp(argv[4], "y") ? 42 : 1;
    static const struct { const char *name; void (*fn)(void); int by_name_only; } tests[] = {
        { "restart", test_sa_restart, 0 }, { "pipe", test_pipe_capacity, 0 }, { "dir", test_open_directory, 0 },
        { "select", test_select, 0 }, { "socketpair", test_socketpair, 0 },
        { "script", test_exec_script, 0 }, { "statfs", test_statfs_links, 0 },
        { "sigchld", test_sigchld, 0 }, { "rlimit", test_rlimit, 0 },
        { "fpu", test_fpu, 0 }, { "loopback", test_loopback, 0 }, { "stream", test_loopback_stream, 0 },
        { "smallwrites", test_tcp_small_writes, 0 },
        { "exitrace", test_exit_preempted, 0 }, { "pollalign", test_poll_unaligned, 0 }, { "killwalk", test_kill_walk, 0 },
        { "vdso", test_vdso, 0 },
        { "oom", test_oom, 1 },                     /* needs a small machine: run as "kernel_test oom" */
    };

    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++)
        if (argc < 2 ? !tests[i].by_name_only : !strcmp(argv[1], tests[i].name)) tests[i].fn();
    return check_report("kernel_test");
}
