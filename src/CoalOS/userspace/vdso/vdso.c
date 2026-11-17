/*
 * The vDSO image: __kernel_vsyscall (the sysenter entry) and the time functions that read the
 * kernel's time page instead of entering the kernel. Linked at VDSO_BASE by src/arch/i386/vdso.ld.
 * Functions follow the Linux i386 vDSO signatures; they return 0 or -errno from the fallback call.
 */
#include <kernel/cpu/vdso_abi.h>

#define NR_TIME           13
#define NR_GETTIMEOFDAY   78
#define NR_CLOCK_GETTIME  265
#define CLOCK_REALTIME    0
#define CLOCK_MONOTONIC   1

struct timespec { int32_t tv_sec, tv_nsec; };
struct timeval  { int32_t tv_sec, tv_usec; };

/* Called by user code with the registers set up as for int 0x80; see vdso_abi.h for the protocol. */
__asm__(".section .text.vsyscall, \"ax\"\n"
        ".globl __kernel_vsyscall\n"
        ".type __kernel_vsyscall, @function\n"
        "__kernel_vsyscall:\n"
        "   push %ecx\n"
        "   push %edx\n"
        "   push %ebp\n"
        "   mov  %esp, %ebp\n"
        "   sysenter\n"
        "   int  $0x80\n"
        "1: pop  %ebp\n"
        "   pop  %edx\n"
        "   pop  %ecx\n"
        "   ret\n"
        ".if 1b - __kernel_vsyscall != 9\n"
        ".error \"VDSO_LANDING in vdso_abi.h does not match the stub\"\n"
        ".endif\n"
        ".size __kernel_vsyscall, . - __kernel_vsyscall\n"
        ".text\n");

static long fallback(long nr, long a, long b) {
    long r;

    __asm__ volatile("int $0x80" : "=a"(r) : "0"(nr), "b"(a), "c"(b) : "memory");
    return r;
}

/* Nanoseconds on the given clock from the time page; 0 if the caller must ask the kernel. */
static inline __attribute__((always_inline)) int clock_ns(int clk, uint64_t *ns) {
    const volatile struct vdso_data *d = (const volatile struct vdso_data *)VVAR_BASE;
    const volatile uint64_t *base_ns = clk == CLOCK_REALTIME ? &d->real_base_ns : &d->mono_base_ns;
    uint32_t seq, mult, shift;
    uint64_t base, tsc0, tsc;

    for (;;) {
        seq = d->seq;
        __asm__ volatile("" ::: "memory");
        mult = d->mult;
        shift = d->shift;
        tsc0 = d->tsc_base;
        base = *base_ns;
        __asm__ volatile("rdtsc" : "=A"(tsc));
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
    uint32_t rem;

    if ((clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) || !clock_ns(clk, &ns))
        return (int)fallback(NR_CLOCK_GETTIME, clk, (long)ts);
    ts->tv_sec = (int32_t)vdso_split_ns(ns, &rem);
    ts->tv_nsec = (int32_t)rem;
    return 0;
}

int __vdso_gettimeofday(struct timeval *tv, int32_t *tz) {
    uint64_t ns;
    uint32_t rem;

    if (!clock_ns(CLOCK_REALTIME, &ns))
        return (int)fallback(NR_GETTIMEOFDAY, (long)tv, (long)tz);
    if (tv) {
        tv->tv_sec = (int32_t)vdso_split_ns(ns, &rem);
        tv->tv_usec = (int32_t)(rem / 1000);
    }
    if (tz) tz[0] = tz[1] = 0;                  /* the zone is UTC */
    return 0;
}

int32_t __vdso_time(int32_t *t) {
    uint64_t ns;
    uint32_t rem, sec;

    if (!clock_ns(CLOCK_REALTIME, &ns))
        return (int32_t)fallback(NR_TIME, (long)t, 0);
    sec = vdso_split_ns(ns, &rem);
    if (t) *t = (int32_t)sec;
    return (int32_t)sec;
}
