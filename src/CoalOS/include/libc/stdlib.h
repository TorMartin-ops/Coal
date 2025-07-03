#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include "stddef.h"

// Exit status codes
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// Maximum value for rand()
#define RAND_MAX 32767

// Heap size for simple allocator
#define HEAP_SIZE (64 * 1024)  // 64KB heap

// Maximum environment variables
#define MAX_ENV_VARS 64

// Memory allocation
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

// String conversion
int atoi(const char *str);
long atol(const char *str);
long long atoll(const char *str);
long strtol(const char *str, char **endptr, int base);
unsigned long strtoul(const char *str, char **endptr, int base);
long long strtoll(const char *str, char **endptr, int base);
unsigned long long strtoull(const char *str, char **endptr, int base);

// Random number generation
int rand(void);
void srand(unsigned int seed);

// Environment
extern char **environ;
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

// Process control
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*function)(void));
int system(const char *command);

// Absolute value
int abs(int j);
long labs(long j);
long long llabs(long long j);

// Division
typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

// Searching and sorting
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif // LIBC_STDLIB_H