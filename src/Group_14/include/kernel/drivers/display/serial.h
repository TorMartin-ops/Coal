#ifndef SERIAL_H
#define SERIAL_H

#include <kernel/core/types.h>

// COM1 Port Base
#define SERIAL_COM1_BASE 0x3F8

// Function prototypes
void serial_init(); // Optional initialization
void serial_putchar(char c);
void serial_write(const char *str);
void serial_print_hex(uint32_t n); // Print hex value
void serial_printf(const char *fmt, ...); // Printf-style serial output

#endif // SERIAL_H