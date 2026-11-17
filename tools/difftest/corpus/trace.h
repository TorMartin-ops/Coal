/* Canonical trace helpers shared by the corpus. Programs call the kernel through raw system calls (a
 * negative return is -errno, no libc state involved) and print one line per call:
 *     <label>: <result> [detail]
 * where <label> starts with the system call name. Values that differ between runs (pids, addresses,
 * times) are normalised by the program before they are printed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static long sc_(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define N_(_1, _2, _3, _4, _5, _6, _7, N, ...) N
#define C_(a, b) a##b
#define CC_(a, b) C_(a, b)
#define L_(x) ((long)(x))
#define K(...) CC_(K_, N_(__VA_ARGS__, 6, 5, 4, 3, 2, 1, 0))(__VA_ARGS__)
#define K_0(n) sc_(L_(n), 0, 0, 0, 0, 0, 0)
#define K_1(n, a) sc_(L_(n), L_(a), 0, 0, 0, 0, 0)
#define K_2(n, a, b) sc_(L_(n), L_(a), L_(b), 0, 0, 0, 0)
#define K_3(n, a, b, c) sc_(L_(n), L_(a), L_(b), L_(c), 0, 0, 0)
#define K_4(n, a, b, c, d) sc_(L_(n), L_(a), L_(b), L_(c), L_(d), 0, 0)
#define K_5(n, a, b, c, d, e) sc_(L_(n), L_(a), L_(b), L_(c), L_(d), L_(e), 0)
#define K_6(n, a, b, c, d, e, f) sc_(L_(n), L_(a), L_(b), L_(c), L_(d), L_(e), L_(f))

static const char *errname(long e) {
    switch (e) {
#define E(x) case x: return #x;
    E(EPERM) E(ENOENT) E(ESRCH) E(EINTR) E(EIO) E(ENXIO) E(E2BIG) E(ENOEXEC) E(EBADF) E(ECHILD) E(EAGAIN)
    E(ENOMEM) E(EACCES) E(EFAULT) E(EBUSY) E(EEXIST) E(EXDEV) E(ENODEV) E(ENOTDIR) E(EISDIR) E(EINVAL)
    E(ENFILE) E(EMFILE) E(ENOTTY) E(ETXTBSY) E(EFBIG) E(ENOSPC) E(ESPIPE) E(EROFS) E(EMLINK) E(EPIPE)
    E(EDOM) E(ERANGE) E(EDEADLK) E(ENAMETOOLONG) E(ENOSYS) E(ENOTEMPTY) E(ELOOP) E(ENOTSOCK)
    E(EDESTADDRREQ) E(EMSGSIZE) E(EPROTOTYPE) E(ENOPROTOOPT) E(EPROTONOSUPPORT) E(EOPNOTSUPP)
    E(EAFNOSUPPORT) E(EADDRINUSE) E(EADDRNOTAVAIL) E(ENETUNREACH) E(ECONNABORTED) E(ECONNRESET)
    E(ENOBUFS) E(EISCONN) E(ENOTCONN) E(ETIMEDOUT) E(ECONNREFUSED) E(EALREADY) E(EINPROGRESS)
    E(ESOCKTNOSUPPORT) E(EPFNOSUPPORT) E(EHOSTUNREACH) E(EOVERFLOW) E(EBADFD)
#undef E
    default: return "E?";
    }
}

static void put_(const char *s, size_t n) {
    while (n) {
        long w = sc_(SYS_write, 1, (long)s, (long)n, 0, 0, 0);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

/* One result: a negative errno becomes its name, anything else a decimal number. */
static void tr(const char *label, long r, const char *fmt, ...) {
    char b[600];
    int n = 0;
    n += snprintf(b + n, sizeof b - (size_t)n, "%s: ", label);
    if (r < 0 && r >= -4095) n += snprintf(b + n, sizeof b - (size_t)n, "%s", errname(-r));
    else n += snprintf(b + n, sizeof b - (size_t)n, "%ld", r);
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        n += snprintf(b + n, sizeof b - (size_t)n, " ");
        n += vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap);
        va_end(ap);
    }
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    put_(b, (size_t)n);
}

/* R(label, call): evaluate, print, yield the result. */
#define R(label, call) ({ long r_ = (call); tr(label, r_, NULL); r_; })
/* RF(label, call, fmt, ...): same with detail evaluated after the call. */
#define RF(label, call, ...) ({ long r_ = (call); tr(label, r_, __VA_ARGS__); r_; })
/* Free-form line (a derived fact, never a raw system call result). */
#define NOTE(...) do { char b_[300]; int n_ = snprintf(b_, sizeof b_, __VA_ARGS__); b_[n_++] = '\n'; put_(b_, (size_t)n_); } while (0)

/* A value that is only checked for shape. */
static const char *yn(int c) { return c ? "yes" : "no"; }
/* Normalise a pid: whether it is positive, and whether it is the expected one. */
static const char *pidn(long v) { return v > 0 ? "PID" : v == 0 ? "0" : "ERR"; }

/* Start-up: close inherited descriptors and clear the umask so runs on both systems begin alike. */
static void init(void) {
    for (int i = 3; i < 64; i++) sc_(SYS_close, i, 0, 0, 0, 0, 0);
    sc_(SYS_umask, 022, 0, 0, 0, 0, 0);
}

static void rmtree_(const char *path);
/* Scratch directory unique to the program, removed and recreated, set as cwd. */
static char scratch_[64];
static void scratch(const char *name) {
    snprintf(scratch_, sizeof scratch_, "/tmp/dt_%s", name);
    rmtree_(scratch_);
    sc_(SYS_mkdir, (long)scratch_, 0755, 0, 0, 0, 0);
    sc_(SYS_chdir, (long)scratch_, 0, 0, 0, 0, 0);
}

struct dent_ { uint64_t ino, off; unsigned short len; unsigned char type; char name[256]; };
static void rmtree_(const char *path) {
    long fd = sc_(SYS_open, (long)path, O_RDONLY | O_DIRECTORY, 0, 0, 0, 0);
    if (fd < 0) { sc_(SYS_unlink, (long)path, 0, 0, 0, 0, 0); return; }
    static char buf[1024];
    for (;;) {
        long n = sc_(SYS_getdents64, fd, (long)buf, sizeof buf, 0, 0, 0);
        if (n <= 0) break;
        for (long o = 0; o < n;) {
            struct dent_ *d = (struct dent_ *)(buf + o);
            o += d->len;
            if (!strcmp(d->name, ".") || !strcmp(d->name, "..")) continue;
            char sub[300];
            snprintf(sub, sizeof sub, "%s/%s", path, d->name);
            if (d->type == 4) rmtree_(sub); else sc_(SYS_unlink, (long)sub, 0, 0, 0, 0, 0);
        }
    }
    sc_(SYS_close, fd, 0, 0, 0, 0, 0);
    sc_(SYS_rmdir, (long)path, 0, 0, 0, 0, 0);
}

/* Kernel struct stat (x86_64 layout) so no libc translation sits between program and kernel. */
struct kstat_ { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks;
                int64_t at, atn, mt, mtn, ct, ctn; int64_t unused[3]; };
static const char *ftype(uint32_t m) {
    switch (m & 0170000) { case 0100000: return "file"; case 040000: return "dir"; case 0120000: return "symlink";
    case 010000: return "fifo"; case 0140000: return "sock"; case 020000: return "chr"; case 060000: return "blk"; }
    return "?";
}

/* stat family result: file type, permission bits, link count, size, owner. */
static void stshow_(const char *label, long call, struct kstat_ *s) {
    if (call < 0) tr(label, call, NULL);
    else tr(label, call, "type=%s mode=%04o nlink=%lu size=%ld uid=%u gid=%u", ftype(s->mode), s->mode & 07777,
            (unsigned long)s->nlink, (long)s->size, s->uid, s->gid);
}
#define STAT(label, path) do { struct kstat_ s_; memset(&s_, 0xa5, sizeof s_); stshow_(label, K(SYS_stat, path, &s_), &s_); } while (0)
#define LSTAT(label, path) do { struct kstat_ s_; memset(&s_, 0xa5, sizeof s_); stshow_(label, K(SYS_lstat, path, &s_), &s_); } while (0)
#define FSTAT(label, fd) do { struct kstat_ s_; memset(&s_, 0xa5, sizeof s_); stshow_(label, K(SYS_fstat, fd, &s_), &s_); } while (0)
/* Read up to n bytes and show them (printable) after a result line. */
static void rd(const char *label, long fd, size_t n) {
    char b[256];
    if (n > sizeof b) n = sizeof b;
    long r = K(SYS_read, fd, b, n);
    if (r > 0) { for (long i = 0; i < r; i++) if (b[i] < 32 || b[i] > 126) b[i] = '.'; tr(label, r, "\"%.*s\"", (int)r, b); }
    else tr(label, r, NULL);
}

/* Set a signal to SIG_IGN with the raw call (the kernel's struct sigaction, 8-byte mask). */
static void ignsig(int sig) {
    struct { void *h; unsigned long f; void *r; unsigned long m; } a = {(void *)1, 0, 0, 0};
    sc_(SYS_rt_sigaction, sig, (long)&a, 0, 8, 0, 0);
}
