#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <libc/sys/types.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

typedef int clockid_t;
struct timespec { long tv_sec; long tv_nsec; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

time_t time(time_t *t);
int clock_gettime(clockid_t clk, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
struct tm *gmtime_r(const time_t *t, struct tm *out);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif
