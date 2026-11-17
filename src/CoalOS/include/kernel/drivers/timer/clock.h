/**
 * @file clock.h
 * @brief System clocks with nanosecond resolution, derived from the TSC.
 *
 * Both clocks start from one TSC reading taken at boot and a rate computed from the PIT calibration
 * (pit.c). CLOCK_MONOTONIC is the time since timer_early_init(); CLOCK_REALTIME adds the CMOS clock
 * read once at boot. Without a usable TSC both fall back to the 1 ms tick. The same numbers are
 * published to user space in the vDSO time page (vdso_abi.h), so a user-space read matches a
 * system call.
 */
#ifndef KERNEL_DRIVERS_TIMER_CLOCK_H
#define KERNEL_DRIVERS_TIMER_CLOCK_H

#include <kernel/core/types.h>
#include <kernel/cpu/vdso_abi.h>

/** Derives the TSC scale factors; called by timer_early_init() so that clock_ns() works from the start. */
void clock_calibrate(void);

/** Reads the CMOS clock for CLOCK_REALTIME. Call after timer_early_init(). */
void clock_init(void);

uint64_t clock_monotonic_ns(void);
uint64_t clock_realtime_ns(void);

/** Writes the current clock parameters into the vDSO time page, under its seqlock. */
void clock_publish(volatile struct vdso_data *page);

#endif
