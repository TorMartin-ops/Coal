/*
 * Minimal checks for the user-space test programs.
 * CHECK counts failures and prints them; test programs return failures() != 0.
 */
#ifndef USER_TESTS_CHECK_H
#define USER_TESTS_CHECK_H

#include <libc/stdio.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                   \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                             \
    } while (0)

static int check_report(const char *name) {
    printf("%s: %d checks, %d failed\n", name, g_checks, g_failures);
    return g_failures != 0;
}

#endif /* USER_TESTS_CHECK_H */
