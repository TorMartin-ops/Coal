/**
 * @file rtc.h
 * @brief Wall clock: the CMOS RTC (UTC) is read once, then advanced by the PIT tick.
 */
#ifndef KERNEL_DRIVERS_TIMER_RTC_H
#define KERNEL_DRIVERS_TIMER_RTC_H

#include <kernel/core/types.h>

/** Current Unix time in seconds. */
uint32_t wall_seconds(void);

/** Current Unix time split into seconds and nanoseconds (millisecond resolution). */
void wall_time(uint32_t *sec, uint32_t *nsec);

#endif
