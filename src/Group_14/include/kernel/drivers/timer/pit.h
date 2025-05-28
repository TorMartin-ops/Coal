#ifndef PIT_H
#define PIT_H

#include <kernel/core/types.h> // Includes libc/stdint.h and libc/stdbool.h via types.h

/**
 * PIT (Programmable Interval Timer) I/O port addresses
 */
#define PIT_CMD_PORT         0x43
#define PIT_CHANNEL0_PORT    0x40
#define PIT_CHANNEL1_PORT    0x41
#define PIT_CHANNEL2_PORT    0x42
#define PC_SPEAKER_PORT      0x61

/**
 * Default divisor if you want ~18.2 Hz:
 * PIT_DEFAULT_DIVISOR = 0x4E20 (20000 decimal),
 * 1193180 / 20000 ≈ 18.2 Hz
 *
 * Not strictly needed if you reprogram the PIT with a custom frequency.
 */
#define PIT_DEFAULT_DIVISOR  0x4E20

/**
 * PIC (Programmable Interrupt Controller) macros, if needed
 */
#define PIC1_CMD_PORT        0x20
#define PIC1_DATA_PORT       0x21
#define PIC_EOI              0x20  /* End-of-interrupt command code */

/**
 * Custom frequency definitions:
 * PIT_BASE_FREQUENCY = 1193180 Hz (original PIT clock)
 * TARGET_FREQUENCY   = 1000 Hz by default => 1ms per tick
 * DIVIDER            = PIT_BASE_FREQUENCY / TARGET_FREQUENCY
 * TICKS_PER_MS       = 1 if freq=1000 (TARGET_FREQUENCY / 1000)
 */
#define PIT_BASE_FREQUENCY   1193180
#ifndef TARGET_FREQUENCY // Allow override via build flags if needed
#define TARGET_FREQUENCY     1000
#endif
#define DIVIDER              (PIT_BASE_FREQUENCY / TARGET_FREQUENCY)
#define TICKS_PER_MS         (TARGET_FREQUENCY / 1000)

/**
 * init_pit
 *
 * Initializes the PIT:
 * - Installs an IRQ0 handler to increment a global tick counter and call schedule().
 * - Sets the PIT to TARGET_FREQUENCY (e.g. 1000 Hz => 1ms per tick).
 *
 * Call this in your kernel_main (or similar) to enable timing and preemption.
 */
void init_pit(void);

/**
 * get_pit_ticks
 *
 * Returns how many PIT ticks have elapsed since init_pit() was called.
 * If frequency=1000 Hz, each tick = 1 ms.
 */
uint32_t get_pit_ticks(void);

/**
 * sleep_busy
 *
 * Sleeps for 'milliseconds' using a busy-wait loop (high CPU usage).
 * Checks get_pit_ticks() in a tight loop until enough ticks pass.
 */
void sleep_busy(uint32_t milliseconds);

/**
 * sleep_interrupt
 *
 * Sleeps for 'milliseconds' using interrupts (low CPU usage).
 * In a loop, we enable interrupts (sti) then halt (hlt) until next PIT tick,
 * checking get_pit_ticks() after each interrupt until time is up.
 * Note: This is a basic implementation and might interact poorly with
 * complex scheduling or interrupt handling.
 */
void sleep_interrupt(uint32_t milliseconds);

/* Functions removed as the scheduler now controls its own readiness:
 * - pit_set_scheduler_ready()
 * - pit_is_scheduler_ready()
 */

#endif // PIT_H