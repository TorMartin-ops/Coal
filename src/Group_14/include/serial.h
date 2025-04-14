#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

// COM1 Port Base
#define SERIAL_COM1_BASE 0x3F8

// Function prototypes
void serial_init(); // Optional initialization
void serial_putchar(char c);
void serial_write(const char *str);
// You could also implement serial_printf if needed

#endif // SERIAL_H