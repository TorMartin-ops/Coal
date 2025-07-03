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
bool print(const char* data, size_t length);

// Formatted output
int printf(const char* format, ...);
int vprintf(const char* format, va_list args);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsprintf(char *str, const char *format, va_list args);
int vsnprintf(char *str, size_t size, const char *format, va_list args);
int fprintf(FILE *stream, const char *format, ...);

// File operations (minimal for now)
FILE *fopen(const char *pathname, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fflush(FILE *stream);

#endif // LIBC_STDIO_H