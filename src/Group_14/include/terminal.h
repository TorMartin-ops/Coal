#ifndef TERMINAL_H
#define TERMINAL_H

#include "libc/stdint.h"

void terminal_init(void);
void terminal_write(const char *string);

#endif // TERMINAL_H
