/**
 * @file terminal.h
 * @brief VGA text console with a VT100 subset. Screen only: kernel messages go
 *        through klog (log.h), which also feeds COM1 and the log ring.
 */
#ifndef TERMINAL_H
#define TERMINAL_H

#include <kernel/core/types.h>
#include <libc/stdarg.h>

void terminal_init(void);
void terminal_write_bytes(const char *data, size_t size);

/** Kernel snprintf (format.c); returns the number of characters stored. */
int _vsnprintf(char *str, size_t size, const char *fmt, va_list args);

#endif /* TERMINAL_H */
