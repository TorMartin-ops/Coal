/**
 * @file pit.h
 * @brief 8253/8254 PIT: 1000 Hz system tick, monotonic tick counter, uptime.
 */
#ifndef PIT_H
#define PIT_H

#include <kernel/core/types.h>

#define TIMER_HZ 1000u /* ticks per second; one tick is 1 ms */

/** Calibrates the TSC against the PIT (a few ms) so that uptime is real from the first log line on.
 *  Call first thing at boot; without a TSC the uptime reads 0 until timer_start(). */
void timer_early_init(void);

/** TSC frequency in kHz from that calibration (0: no TSC); *tsc0 is the TSC value at the start of the uptime. */
uint32_t timer_tsc_khz(uint64_t *tsc0);

/** From here the uptime is the interrupt tick count, continuing from the time already passed.
 *  Call right before enabling interrupts. */
void timer_start(void);

/** Programs channel 0 for TIMER_HZ and installs the IRQ0 handler. */
void init_pit(void);

/** Ticks (ms) since timer_early_init(). Monotonic, never wraps in practice (64 bit). */
uint64_t timer_ticks(void);

/** Milliseconds since timer_early_init(). */
uint64_t timer_uptime_ms(void);

/** Calls fn from the tick interrupt every millisecond (one hook; NULL removes it). */
void timer_set_hook(void (*fn)(void));

#endif /* PIT_H */
