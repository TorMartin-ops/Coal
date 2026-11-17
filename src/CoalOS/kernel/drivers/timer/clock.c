/**
 * @file clock.c
 * @brief CLOCK_MONOTONIC and CLOCK_REALTIME from the TSC (see clock.h).
 */
#include <kernel/drivers/timer/clock.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/timer/rtc.h>

#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_SEC  1000000000u
#define MIN_TSC_KHZ   1000u             /* below 1 MHz the calibration is not believable */

static struct vdso_data clk;            /* the parameters; user space reads a copy of them */

static inline uint64_t rdtsc(void) {
    uint64_t tsc;

    asm volatile("rdtsc" : "=A"(tsc));
    return tsc;
}

/* ns = tsc delta * mult >> shift with the largest shift that keeps mult below 2^32 (vdso_scale needs 1..31). */
static bool compute_scale(uint32_t khz) {
    uint32_t shift = 31, mult, rem;

    while (shift && ((uint64_t)NSEC_PER_MSEC << shift >> 32) >= khz) shift--;
    if (!shift) return false;
    asm("divl %4" : "=a"(mult), "=d"(rem)
        : "a"((uint32_t)((uint64_t)NSEC_PER_MSEC << shift)), "d"((uint32_t)((uint64_t)NSEC_PER_MSEC << shift >> 32)), "r"(khz));
    clk.mult = mult;
    clk.shift = shift;
    return true;
}

static uint64_t since_boot_ns(void) {
    if (!clk.mult) return timer_uptime_ms() * NSEC_PER_MSEC;
    return clk.mono_base_ns + vdso_scale(rdtsc() - clk.tsc_base, clk.mult, clk.shift);
}

void clock_init(void) {
    uint32_t khz = timer_tsc_khz(&clk.tsc_base);

    if (khz < MIN_TSC_KHZ || !compute_scale(khz)) clk.mult = 0;
    clk.mono_base_ns = 0;
    clk.real_base_ns = (uint64_t)rtc_unix_time() * NSEC_PER_SEC - since_boot_ns();
}

uint64_t clock_monotonic_ns(void) {
    return since_boot_ns();
}

uint64_t clock_realtime_ns(void) {
    return since_boot_ns() + clk.real_base_ns - clk.mono_base_ns;
}

void clock_publish(volatile struct vdso_data *page) {
    page->seq = clk.seq + 1;            /* odd: readers retry */
    asm volatile("" ::: "memory");
    page->mult = clk.mult;
    page->shift = clk.shift;
    page->tsc_base = clk.tsc_base;
    page->mono_base_ns = clk.mono_base_ns;
    page->real_base_ns = clk.real_base_ns;
    asm volatile("" ::: "memory");
    clk.seq += 2;
    page->seq = clk.seq;
}
