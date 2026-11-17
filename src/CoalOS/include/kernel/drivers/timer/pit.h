/**
 * @file pit.h
 * @brief 8253/8254 PIT: 1000 Hz system tick, monotonic tick counter, uptime.
 */
#ifndef PIT_H
#define PIT_H

#include <kernel/core/types.h>

#define TIMER_HZ 1000u /* ticks per second; one tick is 1 ms */

/** Programs channel 0 for TIMER_HZ and installs the IRQ0 handler. */
void init_pit(void);

/** Ticks since init_pit(). Monotonic, never wraps in practice (64 bit). */
uint64_t timer_ticks(void);

/** Milliseconds since init_pit(). */
uint64_t timer_uptime_ms(void);

/** Calls fn from the tick interrupt every millisecond (one hook; NULL removes it). */
void timer_set_hook(void (*fn)(void));

#endif /* PIT_H */
