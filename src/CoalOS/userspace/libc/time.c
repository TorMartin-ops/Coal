#include <libc/time.h>
#include <libc/stdio.h>
#include <libc/string.h>

struct tm *gmtime_r(const time_t *t, struct tm *tm) {
    long days = *t / 86400, rem = *t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7 + 7) % 7;
    /* civil-from-days, proleptic Gregorian (Howard Hinnant) */
    long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    int mon = (int)(mp < 10 ? mp + 2 : mp - 10);
    long year = yoe + era * 400 + (mon < 2);
    tm->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm->tm_mon = mon;
    tm->tm_year = (int)(year - 1900);
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static const short cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    tm->tm_yday = cum[mon] + tm->tm_mday - 1 + (leap && mon > 1);
    tm->tm_isdst = 0;
    return tm;
}

static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* Supports %a %b %d %e %H %M %S %Y %m %y %j %F %T %Z %% (always UTC). */
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    char tmp[16];
    for (; *fmt && n + 1 < max; fmt++) {
        if (*fmt != '%') { s[n++] = *fmt; continue; }
        switch (*++fmt) {
        case 'a': strcpy(tmp, days[tm->tm_wday]); break;
        case 'b': strcpy(tmp, months[tm->tm_mon]); break;
        case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
        case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
        case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
        case 'y': snprintf(tmp, sizeof tmp, "%02d", tm->tm_year % 100); break;
        case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
        case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
        case 'F': snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'T': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'Z': strcpy(tmp, "UTC"); break;
        default:  tmp[0] = *fmt; tmp[1] = 0; break;
        }
        for (const char *p = tmp; *p && n + 1 < max; p++) s[n++] = *p;
    }
    s[n] = 0;
    return n;
}
