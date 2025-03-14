#ifndef STRING_H
#define STRING_H

#include "libc/stdint.h"

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);

#endif // STRING_H
