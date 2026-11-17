/**
 * @file rtc.h
 * @brief Wall clock: the CMOS RTC (UTC) is read once at boot (clock_init()), then advanced by clock.c.
 */
#ifndef KERNEL_DRIVERS_TIMER_RTC_H
#define KERNEL_DRIVERS_TIMER_RTC_H

#include <kernel/core/types.h>

/** Reads the CMOS clock: Unix time in seconds, 1 s resolution. */
uint32_t rtc_unix_time(void);

/** Current Unix time in seconds. */
uint32_t wall_seconds(void);

/** Current Unix time split into seconds and nanoseconds (nanosecond resolution). */
void wall_time(uint32_t *sec, uint32_t *nsec);

#endif
