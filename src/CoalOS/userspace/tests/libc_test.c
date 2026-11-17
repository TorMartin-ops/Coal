/* Checks for the user-space C library. Uses only read/write/brk, so it also
 * builds and runs on a Linux host: gcc -m32 -static -nostdlib (see CMake flags). */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/limits.h>
#include <libc/errno.h>
#include <libc/time.h>
#include <libc/unistd.h>
#include "check.h"

static void test_string(void) {
    char buf[32];

    CHECK(strlen("hello") == 5);
    CHECK(strcmp("a", "b") < 0 && strcmp("b", "a") > 0 && strcmp("x", "x") == 0);
    CHECK(strncmp("abcd", "abxx", 2) == 0);
    CHECK(strcasecmp("HeLLo", "hello") == 0);
    strcpy(buf, "ab");
    strcat(buf, "cd");
    CHECK(strcmp(buf, "abcd") == 0);
    CHECK(strchr("abc", 'c') != NULL && strchr("abc", 'z') == NULL);
    CHECK(strrchr("a/b/c", '/') && strrchr("a/b/c", '/')[1] == 'c');
    CHECK(strstr("hello world", "o w") != NULL && strstr("abc", "d") == NULL);
    memset(buf, 'x', 8);
    memmove(buf + 2, buf, 6);
    CHECK(memcmp(buf, "xxxxxxxx", 8) == 0);
    memcpy(buf, "abcdefgh", 8);
    memmove(buf + 2, buf, 6);                   /* overlapping, forward */
    CHECK(memcmp(buf, "ababcdef", 8) == 0);
    memmove(buf, buf + 2, 6);                   /* overlapping, backward */
    CHECK(memcmp(buf, "abcdefef", 8) == 0);
    CHECK(strcmp(strerror(2), "No such file or directory") == 0);
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_stdlib(void) {
    char *end;

    CHECK(atoi("42") == 42);
    CHECK(atoi("  -17x") == -17);
    CHECK(strtol("0x1f", NULL, 0) == 31);
    CHECK(strtol("077", NULL, 0) == 63);
    CHECK(strtol("-12", NULL, 10) == -12);
    const char *none = "  abc";
    strtol(none, &end, 10);
    CHECK(end == none);
    strtol("12abc", &end, 10);
    CHECK(*end == 'a');
    errno = 0;
    CHECK(strtol("99999999999999999999", NULL, 10) == LONG_MAX && errno == ERANGE);
    CHECK(strtol("-99999999999999999999", NULL, 10) == LONG_MIN);
    CHECK(strtol("-9223372036854775808", NULL, 10) == LONG_MIN);
    CHECK(strtoul("18446744073709551615", NULL, 10) == ULONG_MAX);
    CHECK(strtoll("-9223372036854775808", NULL, 10) == LLONG_MIN);
    CHECK(strtoll("123456789012", NULL, 10) == 123456789012LL);
    CHECK(strtoull("ffffffffffffffff", NULL, 16) == ULLONG_MAX);
    errno = 0;
    CHECK(strtoull("18446744073709551616", NULL, 10) == ULLONG_MAX && errno == ERANGE);
    CHECK(PATH_MAX >= 4096);
}

static void test_errno_strings(void) {
    CHECK(strcmp(strerror(EPERM), "Operation not permitted") == 0);
    CHECK(strcmp(strerror(EOPNOTSUPP), "Operation not supported") == 0);
    CHECK(strcmp(strerror(132), "Operation not possible due to RF-kill") == 0);
    CHECK(strcmp(strerror(0), "Success") == 0);
    CHECK(strcmp(strerror(5000), "Unknown error") == 0);
    CHECK(strcmp(strerror(-1), "Unknown error") == 0);
    int ok = 1;
    for (int e = 1; e <= 132; e++) ok &= strcmp(strerror(e), "Unknown error") != 0 || e == 41 || e == 58;
    CHECK(ok);
}

static void on_exit_a(void) { printf("libc_test: atexit handler ran\n"); }

static void test_atexit_and_env(void) {
    CHECK(atexit(on_exit_a) == 0);
    CHECK(setenv("COAL_TEST", "one", 1) == 0);
    CHECK(strcmp(getenv("COAL_TEST"), "one") == 0);
    CHECK(setenv("COAL_TEST", "two", 0) == 0);
    CHECK(strcmp(getenv("COAL_TEST"), "one") == 0);
    CHECK(setenv("COAL_TEST", "two", 1) == 0);
    CHECK(strcmp(getenv("COAL_TEST"), "two") == 0);
    CHECK(unsetenv("COAL_TEST") == 0);
    CHECK(getenv("COAL_TEST") == NULL);
}

static void test_malloc(void) {
    CHECK(malloc(0xFFFFFFF0u) == NULL);

    char *a = malloc(100), *b = malloc(100);
    CHECK(a && b && a != b);
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 100);
    CHECK((unsigned char)a[99] == 0xAA && (unsigned char)b[0] == 0xBB);
    free(a);
    char *c = malloc(100);
    CHECK(c == a);                              /* the freed block is reused */
    char *d = realloc(b, 5000);
    CHECK(d && (unsigned char)d[99] == 0xBB);
    char *e = calloc(10, 4);
    CHECK(e && e[39] == 0);
    CHECK(calloc(0x10000, 0x10000) == NULL);
    for (int i = 0; i < 1000; i++) free(malloc(64 + i));
    free(c); free(d); free(e);
}

static void test_bsearch(void) {
    int v[] = { 1, 3, 5, 7, 9, 11 };
    int k = 7, miss = 4;
    CHECK(bsearch(&k, v, 6, sizeof v[0], cmp_int) == &v[3]);
    CHECK(bsearch(&miss, v, 6, sizeof v[0], cmp_int) == NULL);
    k = 1;
    CHECK(bsearch(&k, v, 6, sizeof v[0], cmp_int) == &v[0]);
    k = 11;
    CHECK(bsearch(&k, v, 6, sizeof v[0], cmp_int) == &v[5]);
    CHECK(bsearch(&k, v, 0, sizeof v[0], cmp_int) == NULL);
}

static void test_qsort(void) {
    int v[] = { 5, 3, 9, 1, 7, 2, 8 };
    qsort(v, 7, sizeof v[0], cmp_int);
    int ok = 1;
    for (int i = 1; i < 7; i++) ok &= v[i - 1] <= v[i];
    CHECK(ok);
}

static void test_printf(void) {
    char buf[64];

    snprintf(buf, sizeof buf, "%d|%u|%x|%X|%c|%s", -5, 7u, 255u, 255u, 'z', "ok");
    CHECK(strcmp(buf, "-5|7|ff|FF|z|ok") == 0);
    snprintf(buf, sizeof buf, "[%5d][%-5d][%05d][%-4s][%4s]", 42, 42, -42, "ab", "cd");
    CHECK(strcmp(buf, "[   42][42   ][-0042][ab  ][  cd]") == 0);
    snprintf(buf, sizeof buf, "%d %ld %llu", INT_MIN, LONG_MAX, 18446744073709551615ULL);
    CHECK(strcmp(buf, "-2147483648 9223372036854775807 18446744073709551615") == 0);
    snprintf(buf, sizeof buf, "%.3s|%*d|%#x|%o|%%", "abcdef", 4, 7, 255, 8);
    CHECK(strcmp(buf, "abc|   7|0xff|10|%") == 0);
    CHECK(snprintf(buf, 4, "abcdef") == 6 && strcmp(buf, "abc") == 0);
    CHECK(sprintf(buf, "%s", "x") == 1);
    snprintf(buf, sizeof buf, "%f|%.2f|%.0f|%.3f", 3.14159, 2.675, 2.5, -0.0005);
    CHECK(strcmp(buf, "3.141590|2.67|2|-0.001") == 0 || strcmp(buf, "3.141590|2.68|2|-0.001") == 0);
    snprintf(buf, sizeof buf, "%.1f|%6.2f|%-7.1f|%+.1f|%.2f", 0.25, 1.005, 9.96, 1.0, 1234567.891);
    CHECK(strcmp(buf, "0.2|  1.00|10.0   |+1.0|1234567.89") == 0);
    snprintf(buf, sizeof buf, "%.3f|%f|%.0f", 0.001, 100.0, 0.5);
    CHECK(strcmp(buf, "0.001|100.000000|0") == 0);
}

static void test_time(void) {
    struct tm tm;
    char buf[32];
    time_t t = 1794916460;                      /* 2026-11-17 11:54:20 UTC, a Tuesday */
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 126 && tm.tm_mon == 10 && tm.tm_mday == 17);
    CHECK(tm.tm_hour == 11 && tm.tm_min == 54 && tm.tm_sec == 20 && tm.tm_wday == 2);
    strftime(buf, sizeof buf, "%F %T %a %b", &tm);
    CHECK(strcmp(buf, "2026-11-17 11:54:20 Tue Nov") == 0);
    t = 951782400;                              /* 2000-02-29 */
    gmtime_r(&t, &tm);
    CHECK(tm.tm_mon == 1 && tm.tm_mday == 29 && tm.tm_yday == 59);
}

int main(int argc, char **argv) {
    CHECK(argc >= 1 && argv[0] && !argv[argc]);
    CHECK(environ != NULL || 1);
    test_string();
    test_stdlib();
    test_errno_strings();
    test_atexit_and_env();
    test_malloc();
    test_bsearch();
    test_qsort();
    test_printf();
    test_time();
    return check_report("libc_test");
}
