#include <kernel/drivers/timer/rtc.h>
#include <kernel/drivers/timer/clock.h>
#include <kernel/lib/port_io.h>

/* CMOS real-time clock (UTC) as Unix time. */
static uint32_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

uint32_t rtc_unix_time(void) {
    uint32_t sec, min, hour, day, mon, year, b;

    while (cmos(0x0A) & 0x80) ;                     /* update in progress */
    sec = cmos(0); min = cmos(2); hour = cmos(4);
    day = cmos(7); mon = cmos(8); year = cmos(9);
    b = cmos(0x0B);
    if (!(b & 4)) {                                 /* BCD */
        #define BCD(x) (((x) & 15) + ((x) >> 4) * 10)
        sec = BCD(sec); min = BCD(min); day = BCD(day); mon = BCD(mon); year = BCD(year);
        hour = BCD(hour & 0x7F) | (hour & 0x80);
    }
    if (!(b & 2) && (hour & 0x80)) hour = ((hour & 0x7F) % 12) + 12;   /* 12-hour clock, PM */
    year += 2000;

    static const uint16_t days_before[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint32_t days = 365 * (year - 1970) + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400
                  + days_before[(mon - 1) % 12] + day - 1;
    if (mon > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) days++;
    return ((days * 24 + hour) * 60 + min) * 60 + sec;
}

uint32_t wall_seconds(void) {
    return (uint32_t)(clock_realtime_ns() / 1000000000u);
}

void wall_time(uint32_t *sec, uint32_t *nsec) {
    uint64_t ns = clock_realtime_ns();

    *sec = (uint32_t)(ns / 1000000000u);
    *nsec = (uint32_t)(ns % 1000000000u);
}
