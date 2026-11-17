/**
 * @file lapic.h
 * @brief Local APIC timer as clock event device (TSC-deadline mode if the CPU has it, one-shot otherwise).
 */
#ifndef KERNEL_DRIVERS_TIMER_LAPIC_H
#define KERNEL_DRIVERS_TIMER_LAPIC_H

#include <kernel/drivers/timer/timer.h>

#define LAPIC_TIMER_VECTOR 0x30
#define LAPIC_SPURIOUS_VECTOR 0x3F

/** Enables the local APIC and calibrates its timer against the TSC.
 *  Returns NULL if there is no local APIC or no TSC; the caller then falls back to the PIT. */
const struct clockevent *lapic_clockevent_init(void);

#endif
