#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <libc/stddef.h>
#include <libc/stdarg.h>

typedef int ssize_t;    /* same as the kernel's */

#define EOF (-1)
#define BUFSIZ 1024

typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
int   fflush(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int   fgetc(FILE *f);
int   fputc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
int   fputs(const char *s, FILE *f);
ssize_t getline(char **line, size_t *cap, FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
int   fileno(FILE *f);
int   putchar(int c);
int   puts(const char *s);
#define getc fgetc
#define putc fputc
#define getchar() fgetc(stdin)

int  remove(const char *path);
int  rename(const char *from, const char *to);
void perror(const char *msg);

#endif
