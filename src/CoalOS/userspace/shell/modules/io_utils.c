/**
 * @file io_utils.c
 * @brief I/O utility functions implementation for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#include "io_utils.h"
#include "syscall_wrapper.h"
#include "string_utils.h"

//============================================================================
// Print Function Implementations
//============================================================================

void print_str(const char *s) {
    if (s) sys_puts(s);
}

void print_char(char c) {
    char buf[2] = {c, '\0'};
    sys_puts(buf);
}

void print_int(int n) {
    char buf[12];
    char *p = buf + 11;
    *p = '\0';
    
    bool negative = n < 0;
    if (negative) n = -n;
    
    if (n == 0) {
        *--p = '0';
    } else {
        while (n > 0) {
            *--p = '0' + (n % 10);
            n /= 10;
        }
    }
    
    if (negative) *--p = '-';
    print_str(p);
}

void error(const char *msg) {
    print_str("shell: ");
    print_str(msg);
    print_str("\n");
}

void perror_msg(const char *msg) {
    print_str("shell: ");
    print_str(msg);
    print_str(": operation failed\n");
}

void clear_screen(void) {
    // ANSI escape sequence to clear screen and move cursor to home
    print_str("\033[2J\033[H");
}

void print_colored(const char *color, const char *text) {
    print_str(color);
    print_str(text);
    print_str(COLOR_RESET);
}