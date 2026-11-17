/**
 * @file libc_test.c
 * @brief Checks for the user-space C library. Uses no kernel services except
 *        write(), so it also builds and runs on the host with -m32 -nostdlib.
 */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/limits.h>
#include "check.h"

static void test_string(void) {
    char buf[32];

    CHECK(strlen("hello") == 5);
    CHECK(strcmp("a", "b") < 0);
    CHECK(strncmp("abcd", "abxx", 2) == 0);

    strcpy(buf, "ab");
    strncat(buf, "cdef", 2);
    CHECK(strcmp(buf, "abcd") == 0);
    strncat(buf, "x", 10);          /* shorter than n: must still terminate */
    CHECK(strcmp(buf, "abcdx") == 0);

    CHECK(strchr("abc", 'c') != NULL);
    CHECK(strchr("abc", 'z') == NULL);
}

static void test_stdlib(void) {
    char *end;

    CHECK(atoi("42") == 42);
    CHECK(atoi("  -17x") == -17);
    CHECK(atoll("4294967296") == 4294967296LL);
    CHECK(strtol("0x1f", NULL, 0) == 31);
    CHECK(strtol("077", NULL, 0) == 63);
    CHECK(strtol("99999999999", NULL, 10) == LONG_MAX);

    const char *none = "  abc";
    strtol(none, &end, 10);
    CHECK(end == none);             /* no digits: endptr is the start */
    strtol("12abc", &end, 10);
    CHECK(*end == 'a');

    lldiv_t d = lldiv(1LL << 40, 3);
    CHECK(d.quot == 366503875925LL && d.rem == 1);
    CHECK(lldiv(-7, 2).quot == -3 && lldiv(-7, 2).rem == -1);

    int r = rand();
    CHECK(r >= 0 && r <= RAND_MAX);
}

static void on_exit_a(void) { printf("libc_test: atexit handler ran\n"); }

static void test_atexit_and_env(void) {
    CHECK(atexit(on_exit_a) == 0);   /* the handler prints a line when the program exits */

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
    CHECK(malloc(0xFFFFFFFFu) == NULL);
    CHECK(malloc(HEAP_SIZE) == NULL);       /* larger than what is left */

    char *a = malloc(100);
    char *b = malloc(100);
    CHECK(a && b && a != b);
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 100);
    CHECK((unsigned char)a[99] == 0xAA);
    free(a);
    free(b);

    char *c = calloc(10, 4);
    CHECK(c != NULL && c[39] == 0);
    CHECK(calloc(0x10000, 0x10000) == NULL);
    free(c);
}

static void test_printf(void) {
    char buf[64];

    snprintf(buf, sizeof(buf), "%d|%u|%x|%X|%c|%s", -5, 7u, 255u, 255u, 'z', "ok");
    CHECK(strcmp(buf, "-5|7|ff|FF|z|ok") == 0);
    snprintf(buf, sizeof(buf), "[%5d][%-5d][%05d][%-4s]", 42, 42, -42, "ab");
    CHECK(strcmp(buf, "[   42][42   ][-0042][ab  ]") == 0);
    snprintf(buf, sizeof(buf), "%d", INT_MIN);
    CHECK(strcmp(buf, "-2147483648") == 0);
    CHECK(snprintf(buf, 4, "abcdef") == 3 && strcmp(buf, "abc") == 0);
    CHECK(sprintf(buf, "%s", "x") == 1);
    CHECK(stdin != NULL && stdout != stderr);
}

int main(void) {
    test_string();
    test_stdlib();
    test_atexit_and_env();
    test_malloc();
    test_printf();
    return check_report("libc_test");
}
