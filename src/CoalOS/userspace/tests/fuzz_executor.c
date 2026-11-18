/*
 * fuzz_executor: the guest side of the syscall fuzzer (tools/fuzz).
 *
 * Reads serialized programs from the link (virtio console /dev/hvc0, or fuzz.link= on the kernel command line),
 * runs each in a fresh child process with resource limits and a timeout, and answers with the result of every
 * call. A program is a list of raw system calls whose arguments are constants, results of earlier calls,
 * pointers into a 1 MiB arena, or 32/64-bit values loaded from the arena when the call is made (the descriptors
 * a pipe() stored there). Wire format, all little endian:
 *
 *   request   u32 magic CFZ1, u32 ncalls, u32 ndata, u32 length of everything after this header
 *             ndata  x { u32 offset, u32 length, bytes[length] padded to 8 }     copied into the arena
 *             ncalls x { u16 nr, u8 nargs, u8 pad, nargs x { u8 kind, u8 pad[7], u64 value } }
 *   response  u32 magic CFR1, u32 status, u32 detail, u32 done, done x i64 result
 *             status 0: finished, 1: killed by signal `detail`, 2: timed out, 3: could not run
 *
 * Argument kinds: 0 constant, 1 result of call `value`, 2 arena address + value, 3 u32 at arena + value,
 * 4 u64 at arena + value. fork, clone and vfork run in the child only as a fork whose own child exits at once.
 * Started by init in place of agentd on a fuzzing image.
 */
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>
#include <libc/signal.h>
#include <libc/syscall.h>
#include <libc/sys/wait.h>

#define MAGIC_REQ   0x315a4643u         /* "CFZ1" */
#define MAGIC_RESP  0x3152465au         /* "ZFR1" */
#define ARENA_BASE  0x20000000ul
#define ARENA_SIZE  (1ul << 20)
#define MAX_CALLS   64
#define MAX_REQUEST (256u * 1024)
#define PROG_TIMEOUT_MS 1500

#define SYS_FORK_NR  57
#define SYS_VFORK_NR 58
#define SYS_CLONE_NR 56

struct result_page {
    volatile unsigned done;
    long res[MAX_CALLS];
};

static int link_fd = -1;

static void make_raw(int fd) {
    unsigned t[9];
    __sc(SYS_IOCTL, fd, 0x5401 /* TCGETS */, (long)t, 0, 0);
    t[0] = 0;
    t[1] = 0;
    t[2] = (t[2] & ~0x30u) | 0x30 | 0x80;
    t[3] = 0;
    ((unsigned char *)t)[17 + 6] = 1;
    ((unsigned char *)t)[17 + 5] = 0;
    __sc(SYS_IOCTL, fd, 0x5402 /* TCSETS */, (long)t, 0, 0);
}

static int read_full(void *buf, size_t n) {
    char *p = buf;
    while (n) {
        long r = read(link_fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(const void *buf, size_t n) {
    const char *p = buf;
    int stalls = 0;
    while (n) {
        long w = write(link_fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return -1;
        if (w == 0) {
            if (++stalls > 5000) return -1;
            usleep(1000);
            continue;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void respond(unsigned status, unsigned detail, const struct result_page *rp) {
    unsigned hdr[4] = { MAGIC_RESP, status, detail, rp ? rp->done : 0 };
    write_full(hdr, sizeof hdr);
    if (rp && rp->done) write_full(rp->res, rp->done * sizeof(long));
}

struct call {
    unsigned short nr;
    unsigned char nargs;
    struct { unsigned char kind; unsigned long value; } arg[6];
};

static unsigned long arg_value(const struct call *c, int i, const struct result_page *rp, unsigned idx) {
    unsigned long v = c->arg[i].value;
    switch (c->arg[i].kind) {
    case 1: return v < idx ? (unsigned long)rp->res[v] : (unsigned long)-1;
    case 2: return ARENA_BASE + v;
    case 3: return v + 4 <= ARENA_SIZE ? *(volatile unsigned *)(ARENA_BASE + v) : 0;
    case 4: return v + 8 <= ARENA_SIZE ? *(volatile unsigned long *)(ARENA_BASE + v) : 0;
    default: return v;
    }
}

/* Runs the calls in the child. Anything that would hurt the executor itself is refused or contained. */
static void run_calls(const struct call *calls, unsigned ncalls, struct result_page *rp) {
    for (unsigned i = 0; i < ncalls; i++) {
        const struct call *c = &calls[i];
        unsigned long a[6] = { 0, 0, 0, 0, 0, 0 };
        for (int k = 0; k < c->nargs && k < 6; k++) a[k] = arg_value(c, k, rp, i);
        long r;
        if (c->nr == SYS_FORK_NR || c->nr == SYS_VFORK_NR || c->nr == SYS_CLONE_NR) {
            pid_t p = fork();
            if (p == 0) {
                *(volatile char *)ARENA_BASE = 1;       /* touch a shared page: copy on write */
                _exit(0);
            }
            if (p > 0) waitpid(p, NULL, 0);
            r = p;
        } else {
            r = __sc6(c->nr, (long)a[0], (long)a[1], (long)a[2], (long)a[3], (long)a[4], (long)a[5]);
        }
        rp->res[i] = r;
        rp->done = i + 1;
    }
}

static long now_ms(void) {
    long ts[2];
    __sc(SYS_CLOCK_GETTIME, 1, (long)ts, 0, 0, 0);
    return ts[0] * 1000 + ts[1] / 1000000;
}

static void limits(void) {
    close(link_fd);                     /* a program must not talk to the host link */
    long lim[2];
    lim[0] = lim[1] = 96l << 20;
    __sc(SYS_SETRLIMIT, 9 /* RLIMIT_AS */, (long)lim, 0, 0, 0);
    lim[0] = lim[1] = 4;
    __sc(SYS_SETRLIMIT, 0 /* RLIMIT_CPU */, (long)lim, 0, 0, 0);
    lim[0] = lim[1] = 24;
    __sc(SYS_SETRLIMIT, 7 /* RLIMIT_NOFILE */, (long)lim, 0, 0, 0);
}

static int execute(const unsigned char *req, size_t len, struct result_page *rp) {
    unsigned ncalls, ndata;
    memcpy(&ncalls, req + 4, 4);
    memcpy(&ndata, req + 8, 4);
    if (ncalls > MAX_CALLS) return -1;
    const unsigned char *p = req + 16, *end = req + len;
    static struct call calls[MAX_CALLS];

    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        setpgid(0, 0);
        limits();
        void *m = (void *)__sc6(SYS_MMAP, ARENA_BASE, ARENA_SIZE, 3, 0x32 /* MAP_PRIVATE|ANON|FIXED */, -1, 0);
        if ((unsigned long)m != ARENA_BASE) _exit(120);
        for (unsigned i = 0; i < ndata; i++) {
            unsigned off, n;
            if (p + 8 > end) _exit(121);
            memcpy(&off, p, 4);
            memcpy(&n, p + 4, 4);
            p += 8;
            if ((size_t)(end - p) < n || off > ARENA_SIZE || n > ARENA_SIZE - off) _exit(121);
            memcpy((void *)(ARENA_BASE + off), p, n);
            p += (n + 7u) & ~7u;
        }
        for (unsigned i = 0; i < ncalls; i++) {
            if (p + 4 > end) _exit(122);
            calls[i].nr = (unsigned short)(p[0] | (p[1] << 8));
            calls[i].nargs = p[2];
            p += 4;
            if (calls[i].nargs > 6 || (size_t)(end - p) < 16u * calls[i].nargs) _exit(122);
            for (int k = 0; k < calls[i].nargs; k++) {
                calls[i].arg[k].kind = p[0];
                memcpy(&calls[i].arg[k].value, p + 8, 8);
                p += 16;
            }
        }
        run_calls(calls, ncalls, rp);
        _exit(0);
    }
    long deadline = now_ms() + PROG_TIMEOUT_MS;
    int status = 0;
    for (;;) {
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) break;
        if (now_ms() > deadline) {
            kill(-child, SIGKILL);
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            respond(2, 0, rp);
            goto reap;
        }
        usleep(1000);
    }
    if (WIFSIGNALED(status)) respond(1, (unsigned)WTERMSIG(status), rp);
    else respond(0, (unsigned)WEXITSTATUS(status), rp);
reap:
    while (waitpid(-1, NULL, WNOHANG) > 0) { }      /* children the program orphaned */
    return 0;
}

int main(void) {
    const char *dev = "/dev/hvc0";
    link_fd = open(dev, O_RDWR);
    if (link_fd < 0) {
        dev = "/dev/ttyS1";
        link_fd = open(dev, O_RDWR);
    }
    if (link_fd < 0) return 1;
    make_raw(link_fd);
    __sc(SYS_MKDIR, (long)"/tmp/fz", 0777, 0, 0, 0);
    chdir("/tmp/fz");

    static unsigned char req[MAX_REQUEST];
    struct result_page *rp = (struct result_page *)__sc6(SYS_MMAP, 0, sizeof *rp, 3, 0x21 /* MAP_SHARED|ANON */, -1, 0);
    unsigned hello[4] = { MAGIC_RESP, 100, 0, 0 };
    write_full(hello, sizeof hello);                    /* READY: the host snapshots the machine after this */
    for (;;) {
        unsigned hdr[4];
        if (read_full(hdr, sizeof hdr) < 0) { sleep(1); continue; }
        if (hdr[0] != MAGIC_REQ || hdr[1] > MAX_CALLS) { respond(3, 1, NULL); continue; }
        size_t body_len = hdr[3];
        if (body_len > MAX_REQUEST - 16) { respond(3, 2, NULL); continue; }
        memcpy(req, hdr, 16);
        if (read_full(req + 16, body_len) < 0) { sleep(1); continue; }
        rp->done = 0;
        if (execute(req, 16 + body_len, rp) < 0) respond(3, 3, NULL);
    }
}
