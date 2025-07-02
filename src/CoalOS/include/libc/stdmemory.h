#ifndef STDMEMORY_H
#define STDMEMORY_H

#include <libc/stdint.h>
#include <libc/stddef.h>

void *memcopy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);

#endif // STDMEMORY_H
