/*
 * coalbench: micro-benchmarks for measuring kernel changes.
 *
 * Method and test selection follow lmbench (McVoy & Staelin, "lmbench: Portable
 * tools for performance analysis", USENIX 1996): each sample times a batch of
 * operations with rdtsc, the TSC is calibrated against CLOCK_MONOTONIC at start-up
 * (the kernel clock ticks in milliseconds), the batch is grown until a sample lasts
 * about 10 ms, then warm-up samples are discarded and N timed samples are reduced to
 * median, p10 and p90.
 *
 * Output, one line per test:  name value unit p10=.. p90=.. n=..   (-j: JSON).
 * A test that cannot run on this kernel prints "n/a" as its value.
 */

#include <libc/arpa/inet.h>
#include <libc/errno.h>
#include <libc/fcntl.h>
#include <libc/signal.h>
#include <libc/stdint.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/sys/socket.h>
#include <libc/sys/stat.h>
#include <libc/sys/wait.h>
#include <libc/syscall.h>
#include <libc/sys/time.h>
#include <libc/time.h>
#include <libc/unistd.h>

#define TARGET_NS   10e6     /* grow a batch until one sample lasts this long */
#define BUDGET_NS   4e9      /* upper bound for the timed samples of one test */
#define MIN_REPS    5
#define MAX_REPS    201
#define SAMPLE_PATH "/usr/bin/hello"
#define NULL_PROG   "/usr/bin/coalbench_null"
#define TMP_FILE    "/tmp/coalbench.tmp"
#define CHUNK       65536u
#define FILE_BYTES  (8u << 20)
#define TCP_LAT_PORT 18001
#define TCP_BW_PORT  18002
#define TCP_PROBE    18003
#define TCP_BURST    (1u << 20)

enum { NS, MBS };

/* One call times n operations and returns the elapsed TSC ticks, or 0 if it cannot run. */
struct test {
    const char *name;
    int unit;
    unsigned iters, max_iters;
    unsigned long bytes;                /* per operation, for MB/s */
    int (*setup)(void);
    uint64_t (*run)(unsigned n);
    void (*teardown)(void);
};

static double ns_per_tick;
static int g_reps = 21, g_warmup = 2, g_json;
static char iobuf[CHUNK];

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

#define LOOP(n, ...)                                         \
    do {                                                     \
        uint64_t t0_ = rdtsc();                              \
        for (unsigned i_ = 0; i_ < (n); i_++) { __VA_ARGS__; } \
        return rdtsc() - t0_;                                \
    } while (0)

static double mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double ticks_ns(uint64_t t) {
    return (double)(int64_t)t * ns_per_tick;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double percentile(const double *sorted, int n, double p) {
    double k = p * (n - 1);
    int i = (int)k;
    return i + 1 < n ? sorted[i] + (sorted[i + 1] - sorted[i]) * (k - i) : sorted[i];
}

/* TSC rate from three ~100 ms windows delimited by clock ticks; the median is used. */
static void calibrate(void) {
    double r[3];
    for (int i = 0; i < 3; i++) {
        double t, a, b, e;
        t = mono_ns();
        while ((a = mono_ns()) == t) ;
        uint64_t c0 = rdtsc();
        while ((b = mono_ns()) < a + 100e6) ;
        while ((e = mono_ns()) == b) ;
        uint64_t c1 = rdtsc();
        r[i] = (e - a) / (double)(int64_t)(c1 - c0);
    }
    qsort(r, 3, sizeof r[0], cmp_d);
    ns_per_tick = r[1];
}

/* --- helpers -------------------------------------------------------------------- */

static void *map_anon(size_t len) {
    long r = __sc(SYS_MMAP2, 0, (long)len, 3 /* RW */, 0x22 /* PRIVATE|ANON */, -1);
    return (unsigned long)r > (unsigned long)-4096 ? NULL : (void *)r;
}

static void unmap(void *p, size_t len) {
    __SC3(SYS_MUNMAP, p, len, 0);
}

static void reap(pid_t *pid) {
    if (*pid > 0) {
        kill(*pid, SIGKILL);
        waitpid(*pid, NULL, 0);
    }
    *pid = 0;
}

static void closefd(int *fd) {
    if (*fd > 0) close(*fd);
    *fd = -1;
}

/* --- system call and file system tests ---------------------------------------------- */

static uint64_t t_null_syscall(unsigned n) {
    LOOP(n, __SC1(SYS_GETPPID, 0));
}

static int zero_fd = -1;
static int open_zero(void) { return (zero_fd = open("/dev/zero", O_RDONLY)) < 0; }
static void close_zero(void) { closefd(&zero_fd); }
static uint64_t t_read_zero(unsigned n) {
    LOOP(n, if (read(zero_fd, iobuf, 1) != 1) return 0);
}

static int null_fd = -1;
static int open_null(void) { return (null_fd = open("/dev/null", O_WRONLY)) < 0; }
static void close_null(void) { closefd(&null_fd); }
static uint64_t t_write_null(unsigned n) {
    LOOP(n, if (write(null_fd, iobuf, 1) != 1) return 0);
}

static uint64_t t_gettimeofday(unsigned n) {
    struct timeval tv;
    LOOP(n, if (gettimeofday(&tv, NULL) != 0) return 0);
}

static uint64_t t_clock_gettime(unsigned n) {
    struct timespec ts;
    LOOP(n, if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0);
}

static uint64_t t_open_close(unsigned n) {
    LOOP(n, int fd = open(SAMPLE_PATH, O_RDONLY); if (fd < 0) return 0; close(fd));
}

static uint64_t t_stat(unsigned n) {
    struct stat st;
    LOOP(n, if (stat(SAMPLE_PATH, &st) != 0) return 0);
}

static uint64_t t_pipe_rw(unsigned n) {
    int p[2];
    char c = 'x';
    if (pipe(p)) return 0;
    uint64_t t0 = rdtsc();
    for (unsigned i = 0; i < n; i++)
        if (write(p[1], &c, 1) != 1 || read(p[0], &c, 1) != 1) return 0;
    uint64_t t = rdtsc() - t0;
    close(p[0]);
    close(p[1]);
    return t;
}

static uint64_t create_delete(unsigned n, size_t size) {
    LOOP(n, int fd = open(TMP_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0600);
         if (fd < 0) return 0;
         if (size && write(fd, iobuf, size) != (ssize_t)size) return 0;
         close(fd);
         if (unlink(TMP_FILE)) return 0);
}
static uint64_t t_create_0k(unsigned n) { return create_delete(n, 0); }
static uint64_t t_create_4k(unsigned n) { return create_delete(n, 4096); }

/* --- memory ---------------------------------------------------------------------- */

static uint64_t t_page_fault(unsigned n) {
    char *p = map_anon((size_t)n * 4096);
    if (!p) return 0;
    uint64_t t0 = rdtsc();
    for (unsigned i = 0; i < n; i++) p[(size_t)i * 4096] = 1;
    uint64_t t = rdtsc() - t0;
    unmap(p, (size_t)n * 4096);
    return t;
}

/* A burst of faults after the machine was idle: the kernel's pool of zeroed pages is full. */
static uint64_t t_page_fault_idle(unsigned n) {
    char *p = map_anon((size_t)n * 4096);
    if (!p) return 0;
    struct timespec ts = { 0, 10000000 };
    nanosleep(&ts, NULL);
    uint64_t t0 = rdtsc();
    for (unsigned i = 0; i < n; i++) p[(size_t)i * 4096] = 1;
    uint64_t t = rdtsc() - t0;
    unmap(p, (size_t)n * 4096);
    return t;
}

static void *volatile sink;

static uint64_t t_malloc_free(unsigned n) {
    LOOP(n, void *p = malloc(64); if (!p) return 0; sink = p; free(p));
}

/* 128 live blocks of eight sizes, freed in a different order than allocated. */
static uint64_t t_malloc_churn(unsigned n) {
    static void *blk[128];
    LOOP(n,
        for (unsigned k = 0; k < 128; k++) { blk[k] = malloc(8 + (k & 7) * 40); if (!blk[k]) return 0; }
        for (unsigned k = 0; k < 128; k++) free(blk[(k * 37) & 127]));
}

static uint64_t t_mmap_munmap(unsigned n) {
    LOOP(n, void *p = map_anon(1 << 20); if (!p) return 0; unmap(p, 1 << 20));
}

static char *cp_src, *cp_dst;
static int memcpy_setup(void) {
    cp_src = map_anon(2u << 20);
    cp_dst = map_anon(2u << 20);
    if (!cp_src || !cp_dst) return 1;
    memset(cp_src, 1, 2u << 20);          /* fault the pages in before timing */
    memset(cp_dst, 2, 2u << 20);
    return 0;
}
static void memcpy_teardown(void) {
    if (cp_src) unmap(cp_src, 2u << 20);
    if (cp_dst) unmap(cp_dst, 2u << 20);
    cp_src = cp_dst = NULL;
}
static uint64_t t_memcpy(unsigned n) {
    LOOP(n, memcpy(cp_dst, cp_src, 2u << 20); __asm__ volatile("" ::: "memory"));
}

/* --- processes and IPC ------------------------------------------------------------- */

static uint64_t t_fork_exit(unsigned n) {
    LOOP(n, pid_t pid = fork(); if (pid < 0) return 0; if (!pid) _exit(0); waitpid(pid, NULL, 0));
}

static int null_prog_present(void) { return access(NULL_PROG, 0) != 0; }
static uint64_t t_fork_exec(unsigned n) {
    char *argv[] = { "coalbench_null", NULL };
    LOOP(n, int st = 0; pid_t pid = fork(); if (pid < 0) return 0;
         if (!pid) { execve(NULL_PROG, argv, environ); _exit(127); }
         waitpid(pid, &st, 0);
         if (!WIFEXITED(st) || WEXITSTATUS(st)) return 0);
}

/* lat_ctx: a ring of processes passes a one-byte token through pipes; every hop is one
 * blocking read, one write and one context switch. The cost of the same read+write in a
 * single process is subtracted, as lmbench does. */
#define RING_MAX 8
static int ring_n, ring_fd[RING_MAX][2];
static pid_t ring_pid[RING_MAX];

static void ring_child(int i) {
    char c;
    while (read(ring_fd[i][0], &c, 1) == 1 && write(ring_fd[(i + 1) % ring_n][1], &c, 1) == 1)
        if (c == 'q') _exit(0);
    _exit(1);
}

static int ring_setup(int n) {
    ring_n = n;
    for (int i = 0; i < RING_MAX; i++) ring_fd[i][0] = ring_fd[i][1] = -1;
    for (int i = 0; i < n; i++)
        if (pipe(ring_fd[i])) return 1;
    for (int i = 1; i < n; i++) {
        if ((ring_pid[i] = fork()) < 0) return 1;
        if (!ring_pid[i]) ring_child(i);
    }
    return 0;
}

static void ring_teardown(void) {
    char c = 'q';
    if (ring_pid[1] > 0 && ring_fd[1][1] > 0 && write(ring_fd[1][1], &c, 1) == 1) read(ring_fd[0][0], &c, 1);
    for (int i = 1; i < ring_n; i++) {
        if (ring_pid[i] > 0) { kill(ring_pid[i], SIGKILL); waitpid(ring_pid[i], NULL, 0); }
        ring_pid[i] = 0;
    }
    for (int i = 0; i < ring_n; i++) { closefd(&ring_fd[i][0]); closefd(&ring_fd[i][1]); }
}

static int ctx2_setup(void) { return ring_setup(2); }
static int ctx8_setup(void) { return ring_setup(8); }

static uint64_t t_ctx(unsigned hops) {
    unsigned laps = hops / ring_n;
    char c = 'x';
    uint64_t t0 = rdtsc();
    for (unsigned i = 0; i < laps; i++)
        if (write(ring_fd[1][1], &c, 1) != 1 || read(ring_fd[0][0], &c, 1) != 1) return 0;
    uint64_t ring = rdtsc() - t0, base = t_pipe_rw(laps * ring_n);
    return base && ring > base ? ring - base : 1;
}

static pid_t pbw_pid;
static int pbw_fd = -1;

static int pipe_bw_setup(void) {
    int p[2];
    if (pipe(p)) return 1;
    if ((pbw_pid = fork()) < 0) return 1;
    if (!pbw_pid) {
        close(p[0]);
        while (write(p[1], iobuf, CHUNK) > 0) ;
        _exit(0);
    }
    close(p[1]);
    pbw_fd = p[0];
    return 0;
}
static void pipe_bw_teardown(void) { reap(&pbw_pid); closefd(&pbw_fd); }

/* The writer keeps the pipe full, so a sample reads n chunks of a steady stream. */
static uint64_t t_pipe_bw(unsigned n) {
    unsigned long left = (unsigned long)n * CHUNK;
    uint64_t t0 = rdtsc();
    while (left) {
        ssize_t r = read(pbw_fd, iobuf, left < CHUNK ? left : CHUNK);
        if (r <= 0) return 0;
        left -= (unsigned long)r;
    }
    return rdtsc() - t0;
}

static volatile int sig_hits;
static void on_usr1(int sig) { sig_hits++; }
static int signal_setup(void) { return signal(SIGUSR1, on_usr1) == (sighandler_t)-1; }
static uint64_t t_signal(unsigned n) {
    pid_t me = getpid();
    sig_hits = 0;
    uint64_t t0 = rdtsc();
    for (unsigned i = 0; i < n; i++)
        if (kill(me, SIGUSR1)) return 0;
    uint64_t t = rdtsc() - t0;
    return sig_hits == (int)n ? t : 0;
}

/* --- files ------------------------------------------------------------------- */

static int make_file(int sync_it) {
    int fd = open(TMP_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return -1;
    for (unsigned off = 0; off < FILE_BYTES; off += CHUNK)
        if (write(fd, iobuf, CHUNK) != (ssize_t)CHUNK) { close(fd); return -1; }
    if (sync_it) __SC1(SYS_FSYNC, fd);
    return fd;
}

static uint64_t file_write(int sync_it) {
    unlink(TMP_FILE);
    sync();                                  /* start from a clean cache */
    uint64_t t0 = rdtsc();
    int fd = make_file(sync_it);
    if (fd < 0) return 0;
    close(fd);
    uint64_t t = rdtsc() - t0;
    unlink(TMP_FILE);
    return t;
}
static uint64_t t_file_write(unsigned n) { return file_write(1); }
static uint64_t t_file_write_nosync(unsigned n) { return file_write(0); }

static int file_read_setup(void) {
    int fd = make_file(1);
    if (fd < 0) return 1;
    close(fd);
    sync();
    return 0;
}
static void file_read_teardown(void) { unlink(TMP_FILE); }
static uint64_t t_file_read(unsigned n) {
    uint64_t t0 = rdtsc();
    int fd = open(TMP_FILE, O_RDONLY);
    if (fd < 0) return 0;
    for (unsigned off = 0; off < FILE_BYTES; off += CHUNK)
        if (read(fd, iobuf, CHUNK) != (ssize_t)CHUNK) { close(fd); return 0; }
    close(fd);
    return rdtsc() - t0;
}

/* --- TCP over 127.0.0.1 -------------------------------------------------------- */

static struct sockaddr_in loopback(int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static int listen_on(int port) {
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loopback(port);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 1)) { close(fd); return -1; }
    return fd;
}

static int connect_to(int port) {
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loopback(port);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* Without a loopback interface connect() can block for a long time, so it is tried in a
 * child that is killed after two seconds. */
static int loopback_works(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    int lfd = listen_on(TCP_PROBE);
    if (lfd < 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        int c = connect_to(TCP_PROBE);
        _exit(c < 0);
    }
    int st = 0;
    for (int waited_ms = 0; pid > 0; waited_ms += 5) {
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (WIFEXITED(st) && !WEXITSTATUS(st)) state = 1;
            break;
        }
        if (waited_ms >= 2000) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); break; }
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    close(lfd);
    return state > 0;
}

static pid_t tcp_pid;
static int tcp_fd = -1;

static int tcp_setup(int port, int bulk) {
    if (!loopback_works()) return 1;
    int lfd = listen_on(port);
    if (lfd < 0) return 1;
    if ((tcp_pid = fork()) < 0) { close(lfd); return 1; }
    if (!tcp_pid) {
        int one = 1, c = accept(lfd, NULL, NULL);
        if (c < 0) _exit(1);
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        for (;;) {
            unsigned want = bulk ? TCP_BURST : 1;
            while (want) {
                ssize_t r = read(c, iobuf, want < CHUNK ? want : CHUNK);
                if (r <= 0) _exit(0);
                want -= (unsigned)r;
            }
            if (write(c, iobuf, 1) != 1) _exit(1);      /* echo, or the burst acknowledgement */
        }
    }
    tcp_fd = connect_to(port);
    close(lfd);
    return tcp_fd < 0;
}
static int tcp_lat_setup(void) { return tcp_setup(TCP_LAT_PORT, 0); }
static int tcp_bw_setup(void) { return tcp_setup(TCP_BW_PORT, 1); }
static void tcp_teardown(void) { closefd(&tcp_fd); reap(&tcp_pid); }

static uint64_t t_tcp_lat(unsigned n) {
    char c = 'x';
    LOOP(n, if (write(tcp_fd, &c, 1) != 1 || read(tcp_fd, &c, 1) != 1) return 0);
}

static uint64_t t_tcp_bw(unsigned n) {
    LOOP(n, for (unsigned sent = 0; sent < TCP_BURST; sent += CHUNK)
                if (write(tcp_fd, iobuf, CHUNK) != (ssize_t)CHUNK) return 0;
         if (read(tcp_fd, iobuf, 1) != 1) return 0);
}

/* --- driver -------------------------------------------------------------------- */

static const struct test tests[] = {
    { "null_syscall",    NS,  4096, 1 << 20, 0, NULL, t_null_syscall, NULL },
    { "read_zero_1b",    NS,  2048, 1 << 20, 0, open_zero, t_read_zero, close_zero },
    { "write_null_1b",   NS,  2048, 1 << 20, 0, open_null, t_write_null, close_null },
    { "gettimeofday",    NS,  2048, 1 << 20, 0, NULL, t_gettimeofday, NULL },
    { "clock_gettime",   NS,  2048, 1 << 20, 0, NULL, t_clock_gettime, NULL },
    { "open_close",      NS,   256, 1 << 16, 0, NULL, t_open_close, NULL },
    { "stat",            NS,   256, 1 << 16, 0, NULL, t_stat, NULL },
    { "pipe_rw_1b",      NS,   512, 1 << 16, 0, NULL, t_pipe_rw, NULL },
    { "page_fault",      NS,   512,    8192, 0, NULL, t_page_fault, NULL },
    { "page_fault_idle", NS,    16,      16, 0, NULL, t_page_fault_idle, NULL },
    { "mmap_munmap_1m",  NS,   128, 1 << 16, 0, NULL, t_mmap_munmap, NULL },
    { "fork_exit_wait",  NS,     8,     256, 0, NULL, t_fork_exit, NULL },
    { "fork_exec_wait",  NS,     8,     256, 0, null_prog_present, t_fork_exec, NULL },
    { "ctx_2",           NS,    64, 1 << 16, 0, ctx2_setup, t_ctx, ring_teardown },
    { "ctx_8",           NS,    64, 1 << 16, 0, ctx8_setup, t_ctx, ring_teardown },
    { "pipe_bw",        MBS,    32,     512, CHUNK, pipe_bw_setup, t_pipe_bw, pipe_bw_teardown },
    { "signal_roundtrip", NS,  256, 1 << 16, 0, signal_setup, t_signal, NULL },
    { "create_delete_0k", NS,   16,     256, 0, NULL, t_create_0k, NULL },
    { "create_delete_4k", NS,   16,     256, 0, NULL, t_create_4k, NULL },
    { "file_write_8m",  MBS,     1,       1, FILE_BYTES, NULL, t_file_write, NULL },
    { "file_write_8m_nosync", MBS, 1,     1, FILE_BYTES, NULL, t_file_write_nosync, NULL },
    { "file_read_8m",   MBS,     1,       1, FILE_BYTES, file_read_setup, t_file_read, file_read_teardown },
    { "tcp_latency",     NS,   128, 1 << 16, 0, tcp_lat_setup, t_tcp_lat, tcp_teardown },
    { "tcp_bw",         MBS,     1,      64, TCP_BURST, tcp_bw_setup, t_tcp_bw, tcp_teardown },
    { "malloc_free_64", NS,    256, 1 << 16, 0, NULL, t_malloc_free, NULL },
    { "malloc_churn",    NS,     4,     256, 0, NULL, t_malloc_churn, NULL },
    { "memcpy_bw",      MBS,     2,     256, 2u << 20, memcpy_setup, t_memcpy, memcpy_teardown },
};
#define NTESTS ((int)(sizeof tests / sizeof tests[0]))

struct result { double median, p10, p90; int n; };

static double sample_value(const struct test *t, unsigned iters, uint64_t ticks) {
    double ns = ticks_ns(ticks);
    return t->unit == NS ? ns / iters : (double)t->bytes * iters / ns * 1e3;   /* 1 MB = 10^6 bytes */
}

static int measure(const struct test *t, struct result *r) {
    static double samples[MAX_REPS];
    unsigned iters = t->iters;
    int ok = 0;
    uint64_t e;

    if (t->setup && t->setup()) goto out;
    if (!(e = t->run(iters))) goto out;
    while (ticks_ns(e) < TARGET_NS && iters < t->max_iters) {
        iters = iters * 2 > t->max_iters ? t->max_iters : iters * 2;
        if (!(e = t->run(iters))) goto out;
    }
    for (int w = 0; w < g_warmup; w++)
        if (!(e = t->run(iters))) goto out;

    int reps = g_reps;
    double fit = BUDGET_NS / ticks_ns(e);
    if (fit < reps) reps = fit < MIN_REPS ? (g_reps < MIN_REPS ? g_reps : MIN_REPS) : (int)fit;
    for (int i = 0; i < reps; i++) {
        if (!(e = t->run(iters))) goto out;
        samples[i] = sample_value(t, iters, e);
    }
    qsort(samples, (size_t)reps, sizeof samples[0], cmp_d);
    r->median = percentile(samples, reps, 0.5);
    r->p10 = percentile(samples, reps, 0.1);
    r->p90 = percentile(samples, reps, 0.9);
    r->n = reps;
    ok = 1;
out:
    if (t->teardown) t->teardown();
    return ok;
}

/* The libc printf has no floating point. */
static void fmt(char *b, size_t n, double v) {
    long long ip = (long long)v;
    int frac = (int)((v - (double)ip) * 100.0 + 0.5);
    if (frac >= 100) { ip++; frac -= 100; }
    snprintf(b, n, "%lld.%02d", ip, frac);
}

static void emit(const struct test *t, const struct result *r, int ok, int first) {
    char m[32], lo[32], hi[32];
    const char *unit = t->unit == NS ? "ns" : "MB/s";
    if (ok) { fmt(m, sizeof m, r->median); fmt(lo, sizeof lo, r->p10); fmt(hi, sizeof hi, r->p90); }
    if (g_json) {
        printf("%s\n    {\"name\":\"%s\",\"unit\":\"%s\",", first ? "" : ",", t->name, unit);
        if (ok) printf("\"value\":%s,\"p10\":%s,\"p90\":%s,\"n\":%d}", m, lo, hi, r->n);
        else printf("\"value\":null}");
    } else if (ok) {
        printf("%s %s %s p10=%s p90=%s n=%d\n", t->name, m, unit, lo, hi, r->n);
    } else {
        printf("%s n/a %s\n", t->name, unit);
    }
    fflush(stdout);
}

static int usage(void) {
    fprintf(stderr, "usage: coalbench [-j] [-l] [-n reps] [-w warmup] [test-name-substring ...]\n");
    return 2;
}

int main(int argc, char **argv) {
    int list = 0, opt;
    while ((opt = getopt(argc, argv, "jln:w:")) != -1) {
        if (opt == 'j') g_json = 1;
        else if (opt == 'l') list = 1;
        else if (opt == 'n') g_reps = atoi(optarg);
        else if (opt == 'w') g_warmup = atoi(optarg);
        else return usage();
    }
    if (g_reps < 1 || g_reps > MAX_REPS || g_warmup < 0) return usage();
    if (list) {
        for (int i = 0; i < NTESTS; i++) printf("%s\n", tests[i].name);
        return 0;
    }

    calibrate();
    char mhz[32];
    fmt(mhz, sizeof mhz, 1e3 / ns_per_tick);
    if (g_json) printf("{\"tool\":\"coalbench\",\"reps\":%d,\"warmup\":%d,\"tsc_mhz\":%s,\"tests\":[", g_reps, g_warmup, mhz);
    else printf("# coalbench reps=%d warmup=%d tsc_mhz=%s\n", g_reps, g_warmup, mhz);

    int first = 1;
    for (int i = 0; i < NTESTS; i++) {
        int pick = optind >= argc;
        for (int a = optind; a < argc; a++)
            if (strstr(tests[i].name, argv[a])) pick = 1;
        if (!pick) continue;
        struct result r;
        int ok = measure(&tests[i], &r);
        emit(&tests[i], &r, ok, first);
        first = 0;
    }
    if (g_json) printf("\n]}\n");
    return 0;
}
