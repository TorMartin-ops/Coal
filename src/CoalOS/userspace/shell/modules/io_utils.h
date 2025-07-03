/**
 * @file io_utils.h
 * @brief I/O utility functions for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#ifndef IO_UTILS_H
#define IO_UTILS_H

#include "../shell_types.h"

//============================================================================
// Print Functions
//============================================================================

/**
 * @brief Print a string to stdout
 * @param s String to print
 */
void print_str(const char *s);

/**
 * @brief Print a single character to stdout
 * @param c Character to print
 */
void print_char(char c);

/**
 * @brief Print an integer to stdout
 * @param n Integer to print
 */
void print_int(int n);

/**
 * @brief Print error message with "shell: " prefix
 * @param msg Error message
 */
void error(const char *msg);

/**
 * @brief Print error message with operation context
 * @param msg Operation that failed
 */
void perror_msg(const char *msg);

/**
 * @brief Clear the terminal screen
 */
void clear_screen(void);

/**
 * @brief Print colored text (if terminal supports it)
 * @param color ANSI color code
 * @param text Text to print
 */
void print_colored(const char *color, const char *text);

//============================================================================
// ANSI Color Codes
//============================================================================

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_RESET   "\033[0m"

#endif // IO_UTILS_H