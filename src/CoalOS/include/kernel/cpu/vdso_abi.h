/**
 * @file vdso_abi.h
 * @brief The vDSO: a small ELF shared object the kernel maps into every process, plus the
 *        time page ("vvar") the kernel publishes for it. Shared by the kernel and the vDSO source.
 *
 * Layout (fixed, just above the stack window): [VVAR_BASE, +4K) time page, read-only;
 * [VDSO_BASE, +4K) the ELF image with __kernel_vsyscall and __vdso_{clock_gettime,gettimeofday,time}.
 *
 * __kernel_vsyscall follows the Linux i386 convention: arguments as for int 0x80, it saves ecx, edx
 * and ebp on the user stack and enters the kernel with sysenter, with the user stack pointer in ebp.
 * The kernel returns with sysexit to the instruction after the int 0x80 that follows sysenter (the
 * landing pad), which restores the registers. A system call restarted after a signal re-executes that
 * int 0x80, so the same code also serves CPUs without sysenter.
 */
#ifndef KERNEL_CPU_VDSO_ABI_H
#define KERNEL_CPU_VDSO_ABI_H

#include <libc/stdint.h>

#define VVAR_BASE       0xBFFFE000u
#define VDSO_BASE       0xBFFFF000u
#define VDSO_VSYSCALL   (VDSO_BASE + 0x400)     /* __kernel_vsyscall; the linker script asserts it */
#define VDSO_LANDING    (VDSO_VSYSCALL + 9)     /* push ecx, edx, ebp; mov ebp, esp; sysenter; int 0x80; here */
#define VDSO_PATCH      (VDSO_VSYSCALL + 3)     /* mov ebp, esp; sysenter: 4 bytes replaced when the CPU has no sysenter */

/* Time page. The writer makes seq odd, updates, makes it even again; a reader retries if seq changed. */
struct vdso_data {
    volatile uint32_t seq;
    uint32_t mult, shift;       /* ns = (tsc delta * mult) >> shift, 1 <= shift <= 31; mult 0: no usable TSC, call the kernel */
    uint64_t tsc_base;          /* TSC value that mono_base_ns and real_base_ns belong to */
    uint64_t mono_base_ns;      /* CLOCK_MONOTONIC at tsc_base */
    uint64_t real_base_ns;      /* CLOCK_REALTIME at tsc_base */
};

/** (delta * mult) >> shift for a 64-bit delta: the 96-bit product from two 32x32 -> 64 bit multiplies and a
 *  funnel shift by 1..31. By hand because the compiler turns the C version into a long, spilling sequence. */
static inline uint64_t vdso_scale(uint64_t delta, uint32_t mult, uint32_t shift) {
    uint64_t r;
    uint32_t t, u;

    __asm__("movl %[dhi], %%eax\n\t"
            "mull %[m]\n\t"                   /* edx:eax = hi * mult, worth 2^32 */
            "movl %%eax, %[t]\n\t"
            "movl %%edx, %[u]\n\t"
            "movl %[dlo], %%eax\n\t"
            "mull %[m]\n\t"                   /* edx:eax = lo * mult */
            "addl %[t], %%edx\n\t"            /* the 96-bit product is u:edx:eax */
            "adcl $0, %[u]\n\t"
            "shrdl %%cl, %%edx, %%eax\n\t"
            "shrdl %%cl, %[u], %%edx"
            : "=&A"(r), [t] "=&r"(t), [u] "=&r"(u)
            : [dlo] "rm"((uint32_t)delta), [dhi] "rm"((uint32_t)(delta >> 32)), [m] "r"(mult), "c"(shift)
            : "cc");
    return r;
}

/** Splits ns into whole seconds and the remainder; ns must be below 2^32 * 10^9 (that is, before 2106). */
static inline uint32_t vdso_split_ns(uint64_t ns, uint32_t *rem) {
    uint32_t sec, r;

    __asm__("divl %4" : "=a"(sec), "=d"(r) : "a"((uint32_t)ns), "d"((uint32_t)(ns >> 32)), "r"(1000000000u));
    *rem = r;
    return sec;
}

#endif
