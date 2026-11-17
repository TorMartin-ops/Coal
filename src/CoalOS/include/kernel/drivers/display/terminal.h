/**
 * @file terminal.h
 * @brief VGA text console with a VT100 subset, plus kernel message output.
 *
 * terminal_write/printf/putchar are for kernel messages: they go to the screen
 * and, unless disabled at build time, are mirrored to COM1.
 * terminal_write_bytes is for user data (the console device): screen only.
 */
#ifndef TERMINAL_H
#define TERMINAL_H

#include <kernel/core/types.h>
#include <libc/stdarg.h>

void terminal_init(void);

void terminal_putchar(char c);
void terminal_write(const char *str);
void terminal_write_bytes(const char *data, size_t size);
void terminal_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void terminal_vprintf(const char *format, va_list args);

/** Legacy line read: one line without '\n', NUL-terminated. Use tty_read() instead. */
ssize_t terminal_read_line_blocking(char *kbuf, size_t len);

/** Kernel snprintf (format.c); returns the number of characters stored. */
int _vsnprintf(char *str, size_t size, const char *fmt, va_list args);

#endif /* TERMINAL_H */
