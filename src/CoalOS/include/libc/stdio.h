#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include "stddef.h"
#include "stdbool.h"
#include "stdarg.h"

// File pointer type
typedef struct FILE FILE;

// Standard streams
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

// End of file indicator
#define EOF (-1)

// Standard I/O functions
int putchar(int c);
int puts(const char *s);

// Formatted output
int printf(const char* format, ...);
int vprintf(const char* format, va_list args);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsprintf(char *str, const char *format, va_list args);
int vsnprintf(char *str, size_t size, const char *format, va_list args);
int fprintf(FILE *stream, const char *format, ...);


#endif // LIBC_STDIO_H