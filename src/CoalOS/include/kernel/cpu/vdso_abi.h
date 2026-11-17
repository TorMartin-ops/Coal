/**
 * @file vdso_abi.h
 * @brief The vDSO: a small ELF shared object the kernel maps into every process, plus the
 *        time page ("vvar") the kernel publishes for it. Shared by the kernel and the vDSO source.
 *
 * Layout (fixed, at the top of user space): [VVAR_BASE, +4K) time page, read-only;
 * [VDSO_BASE, +4K) the ELF image with __vdso_{clock_gettime,gettimeofday,time,getcpu}.
 * The stack starts below VVAR_BASE; the page above VDSO_BASE stays unmapped (SYSRET).
 */
#ifndef KERNEL_CPU_VDSO_ABI_H
#define KERNEL_CPU_VDSO_ABI_H

#include <libc/stdint.h>

#define VVAR_BASE       0x00007FFFFFFFD000ul
#define VDSO_BASE       0x00007FFFFFFFE000ul

/* Time page. The writer makes seq odd, updates, makes it even again; a reader retries if seq changed. */
struct vdso_data {
    volatile uint32_t seq;
    uint32_t mult, shift;       /* ns = (tsc delta * mult) >> shift; mult 0: no usable TSC, call the kernel */
    uint64_t tsc_base;          /* TSC value that mono_base_ns and real_base_ns belong to */
    uint64_t mono_base_ns;      /* CLOCK_MONOTONIC at tsc_base */
    uint64_t real_base_ns;      /* CLOCK_REALTIME at tsc_base */
};

static inline uint64_t vdso_scale(uint64_t delta, uint32_t mult, uint32_t shift) {
    return (uint64_t)(((unsigned __int128)delta * mult) >> shift);
}

#endif
