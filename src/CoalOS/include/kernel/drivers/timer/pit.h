/**
 * @file pit.h
 * @brief Time base: the TSC, calibrated against PIT channel 2 at boot, gives the monotonic clock.
 *        Without a TSC the PIT ticks at 1 kHz and counts the time instead. The PIT is also the
 *        fallback clock event device when the CPU has no local APIC.
 */
#ifndef PIT_H
#define PIT_H

#include <kernel/core/types.h>
#include <kernel/drivers/timer/timer.h>

/** Calibrates the TSC against the PIT (a few ms) so that time is real from the first log line on.
 *  Call first thing at boot. */
void timer_early_init(void);

/** Nanoseconds since timer_early_init(). Monotonic. */
uint64_t clock_ns(void);

/** TSC frequency in kHz from that calibration (0: no TSC); *tsc0 is the TSC value at the start of the uptime. */
uint32_t timer_tsc_khz(uint64_t *tsc0);

/** Nanoseconds of the PIT periodic tick count (the clock when there is no TSC). */
uint64_t pit_tick_ns(void);

/** TSC frequency in kHz, 0 when the CPU has no TSC. */
uint32_t clock_tsc_khz(void);

static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/** Milliseconds since timer_early_init(). Monotonic, never wraps in practice (64 bit). */
uint64_t timer_ticks(void);

/** Same as timer_ticks(). */
uint64_t timer_uptime_ms(void);

/** The PIT as clock event device: one-shot with a TSC, 1 kHz periodic without. */
const struct clockevent *pit_clockevent_init(void);

#endif /* PIT_H */
