/* Clocks and sleeping. Only orderings and bounds are compared, never absolute readings. */
#include "trace.h"

struct ts { long s, n; };
static long ns(struct ts t) { return t.s * 1000000000L + t.n; }
static const char *rng(long v, long lo, long hi) { return v < lo ? "LOW" : v > hi ? "HIGH" : "ok"; }

int main(void) {
    init();
    struct ts a, b;
    R("clock_gettime(REALTIME)", K(SYS_clock_gettime, CLOCK_REALTIME, &a));
    NOTE("realtime is after 2020 and before 2100: %s", yn(a.s > 1577836800L && a.s < 4102444800L));
    R("clock_gettime(MONOTONIC)", K(SYS_clock_gettime, CLOCK_MONOTONIC, &a));
    R("clock_gettime(MONOTONIC) again", K(SYS_clock_gettime, CLOCK_MONOTONIC, &b));
    NOTE("monotonic does not go back: %s; nanoseconds in range: %s", yn(ns(b) >= ns(a)), yn(b.n >= 0 && b.n < 1000000000L));
    R("clock_gettime(BOOTTIME)", K(SYS_clock_gettime, CLOCK_BOOTTIME, &a));
    R("clock_gettime(PROCESS_CPUTIME)", K(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, &a));
    R("clock_gettime(THREAD_CPUTIME)", K(SYS_clock_gettime, CLOCK_THREAD_CPUTIME_ID, &a));
    R("clock_gettime(MONOTONIC_COARSE)", K(SYS_clock_gettime, CLOCK_MONOTONIC_COARSE, &a));
    R("clock_gettime(REALTIME_COARSE)", K(SYS_clock_gettime, CLOCK_REALTIME_COARSE, &a));
    R("clock_gettime(MONOTONIC_RAW)", K(SYS_clock_gettime, CLOCK_MONOTONIC_RAW, &a));
    R("clock_gettime(bad clock)", K(SYS_clock_gettime, 99, &a));
    R("clock_gettime(bad ptr)", K(SYS_clock_gettime, CLOCK_REALTIME, 8));
    R("clock_getres(MONOTONIC)", K(SYS_clock_getres, CLOCK_MONOTONIC, &a));
    NOTE("resolution is at most 10ms: %s", yn(a.s == 0 && a.n > 0 && a.n <= 10000000L));
    R("clock_getres(bad clock)", K(SYS_clock_getres, 99, &a));

    struct { long s, u; } tv, tv2;
    R("gettimeofday", K(SYS_gettimeofday, &tv, 0));
    NOTE("gettimeofday agrees with realtime within 5s: %s", yn(({ struct ts r; K(SYS_clock_gettime, CLOCK_REALTIME, &r); r.s - tv.s < 5 && tv.s - r.s < 5; })));
    long t = R("time(NULL) > 2020", K(SYS_time, 0) > 1577836800L ? 0 : -1);
    long tp = 0; K(SYS_time, &tp);
    NOTE("time(&t) stores it: %s", yn(tp > 1577836800L)); (void)t; (void)tv2;

    /* sleeping: elapsed time must lie in [request, request + generous slack] */
    struct ts req = {0, 30000000}, rem = {-1, -1}, t0, t1;
    K(SYS_clock_gettime, CLOCK_MONOTONIC, &t0);
    R("nanosleep(30ms)", K(SYS_nanosleep, &req, &rem));
    K(SYS_clock_gettime, CLOCK_MONOTONIC, &t1);
    NOTE("slept at least 30ms: %s; at most 400ms: %s", yn(ns(t1) - ns(t0) >= 30000000L), yn(ns(t1) - ns(t0) < 400000000L));
    struct ts zero = {0, 0};
    R("nanosleep(0)", K(SYS_nanosleep, &zero, 0));
    struct ts bad1 = {0, 1000000000L}, bad2 = {-1, 0};
    R("nanosleep(nsec too big)", K(SYS_nanosleep, &bad1, 0));
    R("nanosleep(negative sec)", K(SYS_nanosleep, &bad2, 0));
    R("nanosleep(bad ptr)", K(SYS_nanosleep, 8, 0));
    K(SYS_clock_gettime, CLOCK_MONOTONIC, &t0);
    struct ts rel = {0, 20000000};
    R("clock_nanosleep(MONOTONIC rel)", K(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &rel, 0));
    K(SYS_clock_gettime, CLOCK_MONOTONIC, &t1);
    NOTE("clock_nanosleep slept >= 20ms: %s", yn(ns(t1) - ns(t0) >= 20000000L));
    struct ts abs; K(SYS_clock_gettime, CLOCK_MONOTONIC, &abs); abs.n += 20000000; if (abs.n >= 1000000000L) { abs.n -= 1000000000L; abs.s++; }
    R("clock_nanosleep(MONOTONIC abs)", K(SYS_clock_nanosleep, CLOCK_MONOTONIC, 1, &abs, 0));
    struct ts past = {0, 1};
    R("clock_nanosleep(abs in the past)", K(SYS_clock_nanosleep, CLOCK_MONOTONIC, 1, &past, 0));
    R("clock_nanosleep(bad clock)", K(SYS_clock_nanosleep, 99, 0, &rel, 0));
    R("clock_nanosleep(CPUTIME self)", K(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, 0, &zero, 0));

    struct { long ut, st, cut, cst; } tms;
    R("times", K(SYS_times, &tms) != 0 ? 0 : -1);
    struct { struct { long s, u; } ut, st; long rest[14]; } ru;
    R("getrusage(SELF)", K(SYS_getrusage, 0, &ru));
    R("getrusage(CHILDREN)", K(SYS_getrusage, -1, &ru));
    R("getrusage(bad who)", K(SYS_getrusage, 5, &ru));
    struct { long s, u; } it = {0, 0}, itv[2] = {{0, 0}, {0, 0}};
    (void)it;
    R("getitimer(REAL)", K(SYS_getitimer, 0, itv));
    R("getitimer(bad which)", K(SYS_getitimer, 9, itv));
    R("alarm(0)", K(SYS_alarm, 0));
    return 0;
}
