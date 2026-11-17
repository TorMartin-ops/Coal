/*
 * Linux i386 ABI smoke test. Built with -nostdlib -ffreestanding and no libc:
 * every call is a raw int 0x80, the way a static Linux binary makes them.
 */

typedef unsigned int u32;

static long sc(long nr, long a, long b, long c, long d, long e, long f) {
    register long r_ebp __asm__("ebp") = f;
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "r"(r_ebp)
                     : "memory", "cc");
    return ret;
}

#define sc3(nr, a, b, c) sc(nr, (long)(a), (long)(b), (long)(c), 0, 0, 0)

enum { NR_exit = 1, NR_read = 3, NR_write = 4, NR_open = 5, NR_close = 6, NR_lseek = 19,
       NR_getpid = 20, NR_dup2 = 63, NR_brk = 45, NR_ioctl = 54, NR_uname = 122,
       NR_mmap2 = 192, NR_munmap = 91, NR_clock_gettime = 265,
       NR_getuid = 24, NR_gettimeofday = 78 };

static int failures;

static unsigned len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void say(const char *s) { sc3(NR_write, 1, s, len(s)); }

static void check(int ok, const char *what) {
    say(ok ? "ok   " : "FAIL ");
    say(what);
    say("\n");
    if (!ok) failures++;
}

struct utsname { char f[6][65]; };

void abi_main(void) {
    struct utsname u;
    char buf[8];
    int tc[10];

    check(sc3(NR_write, 1, "", 0) == 0, "write of 0 bytes");
    check(sc3(NR_getpid, 0, 0, 0) > 0, "getpid > 0");
    check(sc3(NR_getuid, 0, 0, 0) == 0, "getuid == 0");
    check(sc3(NR_uname, &u, 0, 0) == 0 && same(u.f[0], "Coal"), "uname sysname");
    check(sc3(999, 0, 0, 0) == -38, "unknown syscall returns -ENOSYS");
    check(sc3(NR_write, 1, (void *)0xC0000000, 4) == -14, "write from kernel address is EFAULT");
    check(sc3(NR_write, 99, "x", 1) == -9, "write to bad fd is EBADF");

    check(sc3(NR_ioctl, 1, 0x5401, tc) == 0, "TCGETS on the console (isatty)");
    check(sc3(NR_ioctl, 1, 0x5413, tc) == 0 && (tc[0] & 0xffff) == 25, "TIOCGWINSZ rows");

    long brk0 = sc3(NR_brk, 0, 0, 0);
    long brk1 = sc3(NR_brk, brk0 + 8192, 0, 0);
    check(brk1 == brk0 + 8192, "brk grows by two pages");
    if (brk1 == brk0 + 8192) {
        volatile char *h = (char *)brk0;
        h[0] = 1; h[8191] = 2;
        check(h[0] == 1 && h[8191] == 2, "heap is usable");
    }

    long m = sc(NR_mmap2, 0, 4096, 3, 0x22, -1, 0);
    check(m > 0 && !(m & 4095), "mmap2 anonymous");
    if (m > 0) {
        *(volatile char *)m = 7;
        check(*(volatile char *)m == 7 && sc3(NR_munmap, m, 4096, 0) == 0, "anonymous page usable, munmap");
    }

    int fd = sc3(NR_open, "/usr/bin/abi_test", 0, 0);
    check(fd >= 0, "open own image");
    if (fd >= 0) {
        check(sc3(NR_read, fd, buf, 4) == 4 && buf[0] == 0x7f && buf[1] == 'E', "read ELF magic");
        check(sc3(NR_lseek, fd, 1, 0) == 1, "lseek");
        int fd2 = sc3(NR_dup2, fd, 9, 0);
        check(fd2 == 9 && sc3(NR_read, 9, buf, 3) == 3 && buf[0] == 'E', "dup2 shares the offset");
        check(sc3(NR_close, fd, 0, 0) == 0, "close");
        check(sc3(NR_read, 9, buf, 1) == 1, "dup'd descriptor survives close of the original");
        check(sc3(NR_close, 9, 0, 0) == 0 && sc3(NR_close, 9, 0, 0) == -9, "double close is EBADF");
    }
    check(sc3(NR_open, "/no/such/file", 0, 0) == -2, "open of a missing file is ENOENT");

    int ts[2];
    check(sc3(NR_clock_gettime, 1, ts, 0) == 0, "clock_gettime(MONOTONIC)");
    int t1 = ts[0] * 1000 + ts[1] / 1000000, t2 = t1;
    for (long spin = 0; spin < 50000000 && t2 - t1 < 5; spin++) {
        sc3(NR_clock_gettime, 1, ts, 0);
        t2 = ts[0] * 1000 + ts[1] / 1000000;
    }
    check(t2 - t1 >= 5, "monotonic clock advances");

    say(failures ? "abi_test: FAILED\n" : "abi_test: all passed\n");
    sc3(NR_exit, failures ? 1 : 0, 0, 0);
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile("xor %ebp, %ebp; call abi_main; ud2");
}
