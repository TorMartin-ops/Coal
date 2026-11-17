/*
 * The vDSO image: the time functions read the kernel's time page instead of entering the kernel.
 * Linked at VDSO_BASE by src/arch/x86_64/vdso.ld. Signatures follow the Linux x86_64 vDSO; the time
 * functions fall back to the system call when there is no usable TSC.
 */
#include <kernel/cpu/vdso_abi.h>
#include <libc/syscall_numbers.h>

#define NSEC_PER_SEC      1000000000ull
#define CLOCK_REALTIME    0
#define CLOCK_MONOTONIC   1

struct timespec { int64_t tv_sec, tv_nsec; };
struct timeval  { int64_t tv_sec, tv_usec; };

static long fallback(long nr, long a, long b) {
    long r;

    __asm__ volatile("syscall" : "=a"(r) : "0"(nr), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

/* Nanoseconds on the given clock from the time page; 0 if the caller must ask the kernel. */
static inline __attribute__((always_inline)) int clock_ns(int clk, uint64_t *ns) {
    const volatile struct vdso_data *d = (const volatile struct vdso_data *)VVAR_BASE;
    const volatile uint64_t *base_ns = clk == CLOCK_REALTIME ? &d->real_base_ns : &d->mono_base_ns;
    uint32_t seq, mult, shift;
    uint64_t base, tsc0, tsc;

    for (;;) {
        uint32_t lo, hi;

        seq = d->seq;
        __asm__ volatile("" ::: "memory");
        mult = d->mult;
        shift = d->shift;
        tsc0 = d->tsc_base;
        base = *base_ns;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tsc = ((uint64_t)hi << 32) | lo;
        __asm__ volatile("" ::: "memory");
        if (!(seq & 1) && seq == d->seq) break;
        __asm__ volatile("pause");
    }
    if (!mult) return 0;
    *ns = base + vdso_scale(tsc - tsc0, mult, shift);
    return 1;
}

int __vdso_clock_gettime(int clk, struct timespec *ts) {
    uint64_t ns;

    if ((clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) || !clock_ns(clk, &ns))
        return (int)fallback(SYS_CLOCK_GETTIME, clk, (long)ts);
    ts->tv_sec = ns / NSEC_PER_SEC;
    ts->tv_nsec = ns % NSEC_PER_SEC;
    return 0;
}

int __vdso_gettimeofday(struct timeval *tv, int32_t *tz) {
    uint64_t ns;

    if (!clock_ns(CLOCK_REALTIME, &ns))
        return (int)fallback(SYS_GETTIMEOFDAY, (long)tv, (long)tz);
    if (tv) {
        tv->tv_sec = ns / NSEC_PER_SEC;
        tv->tv_usec = ns % NSEC_PER_SEC / 1000;
    }
    if (tz) tz[0] = tz[1] = 0;                  /* the zone is UTC */
    return 0;
}

long __vdso_time(long *t) {
    uint64_t ns;
    long sec;

    if (!clock_ns(CLOCK_REALTIME, &ns))
        return fallback(SYS_TIME, (long)t, 0);
    sec = ns / NSEC_PER_SEC;
    if (t) *t = sec;
    return sec;
}

/* One CPU: there is nothing to look up. */
long __vdso_getcpu(unsigned *cpu, unsigned *node, void *cache) {
    (void)cache;
    if (cpu) *cpu = 0;
    if (node) *node = 0;
    return 0;
}
