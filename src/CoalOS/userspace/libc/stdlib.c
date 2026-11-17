/**
 * @file stdlib.c
 * @brief Standard library functions for Coal OS userspace
 */

#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/limits.h>
#include <libc/stdbool.h>
#include <libc/stdint.h>

// Simple heap allocator for userspace
static char heap[HEAP_SIZE];
static bool heap_initialized = false;

// Block structure for tracking allocations
typedef struct block {
    size_t size;
    bool free;
    struct block *next;
} block_t;

static block_t *heap_head = NULL;

static void init_heap(void) {
    if (heap_initialized) return;
    
    heap_head = (block_t*)heap;
    heap_head->size = HEAP_SIZE - sizeof(block_t);
    heap_head->free = true;
    heap_head->next = NULL;
    heap_initialized = true;
}

void *malloc(size_t size) {
    if (size == 0 || size > HEAP_SIZE) return NULL;

    init_heap();
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    block_t *current = heap_head;
    
    // Find a free block
    while (current) {
        if (current->free && current->size >= size) {
            // Split block if it's much larger
            if (current->size > size + sizeof(block_t) + 8) {
                block_t *new_block = (block_t*)((char*)current + sizeof(block_t) + size);
                new_block->size = current->size - size - sizeof(block_t);
                new_block->free = true;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->free = false;
            return (char*)current + sizeof(block_t);
        }
        current = current->next;
    }
    
    return NULL; // Out of memory
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    
    // Check for overflow
    if (nmemb > SIZE_MAX / size) return NULL;
    
    size_t total_size = nmemb * size;
    void *ptr = malloc(total_size);
    
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    
    // Find the block for this pointer
    block_t *block = (block_t*)((char*)ptr - sizeof(block_t));
    
    if (block->size >= size) {
        // Block is already large enough
        return ptr;
    }
    
    // Need to allocate new block
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    
    return new_ptr;
}

void free(void *ptr) {
    if (!ptr) return;
    
    block_t *block = (block_t*)((char*)ptr - sizeof(block_t));
    block->free = true;
    
    // Coalesce with next free blocks
    while (block->next && block->next->free) {
        block->size += sizeof(block_t) + block->next->size;
        block->next = block->next->next;
    }
    
    // Coalesce with previous free blocks
    block_t *current = heap_head;
    while (current && current->next != block) {
        current = current->next;
    }
    
    if (current && current->free) {
        current->size += sizeof(block_t) + block->size;
        current->next = block->next;
    }
}

// String conversion

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/*
 * Shared parser behind atoi, atol, atoll and strtol. Returns the magnitude,
 * saturated to ULLONG_MAX on overflow, and sets *negative. If no digits are
 * found, *endptr is set to nptr.
 */
static unsigned long long parse_number(const char *nptr, char **endptr, int base,
                                       int *negative, int *overflow) {
    const char *s = nptr;
    unsigned long long result = 0;
    int any = 0;

    *negative = 0;
    *overflow = 0;
    if (endptr) *endptr = (char *)nptr;
    if (base < 0 || base == 1 || base > 36) return 0;

    while (is_space(*s)) s++;
    if (*s == '-') {
        *negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        digit_value(s[2]) >= 0 && digit_value(s[2]) < 16) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int d = digit_value(*s);
        if (d < 0 || d >= base) break;
        if (result > (ULLONG_MAX - (unsigned long long)d) / (unsigned long long)base) {
            *overflow = 1;
            result = ULLONG_MAX;
        } else if (!*overflow) {
            result = result * (unsigned long long)base + (unsigned long long)d;
        }
        any = 1;
        s++;
    }

    if (any && endptr) *endptr = (char *)s;
    return any ? result : 0;
}

static long long strtoll_internal(const char *nptr, char **endptr, int base) {
    int neg, ovf;
    unsigned long long v = parse_number(nptr, endptr, base, &neg, &ovf);
    if (neg) {
        if (v > (unsigned long long)LLONG_MAX + 1ULL) return LLONG_MIN;
        return (long long)(0ULL - v);
    }
    if (v > (unsigned long long)LLONG_MAX) return LLONG_MAX;
    return (long long)v;
}

long strtol(const char *str, char **endptr, int base) {
    long long v = strtoll_internal(str ? str : "", endptr, base);
    if (!str && endptr) *endptr = (char *)str;
    if (v > LONG_MAX) return LONG_MAX;
    if (v < LONG_MIN) return LONG_MIN;
    return (long)v;
}

int atoi(const char *str) {
    return (int)strtol(str, NULL, 10);
}

long atol(const char *str) {
    return strtol(str, NULL, 10);
}

long long atoll(const char *str) {
    return strtoll_internal(str ? str : "", NULL, 10);
}

// Random number generation (simple LCG)
static unsigned int next_rand = 1;

int rand(void) {
    next_rand = next_rand * 1103515245 + 12345;
    return (int)((next_rand / 65536) % (RAND_MAX + 1));
}

void srand(unsigned int seed) {
    next_rand = seed;
}

// Environment variables, stored as "NAME=value" strings.
static char *environ_storage[MAX_ENV_VARS];
static int environ_count = 0;

char **environ = environ_storage;

/* Index of the variable called name, or -1. */
static int env_find(const char *name, size_t name_len) {
    for (int i = 0; i < environ_count; i++) {
        if (strncmp(environ_storage[i], name, name_len) == 0 &&
            environ_storage[i][name_len] == '=') {
            return i;
        }
    }
    return -1;
}

char *getenv(const char *name) {
    if (!name) return NULL;
    size_t name_len = strlen(name);
    int i = env_find(name, name_len);
    return i < 0 ? NULL : environ_storage[i] + name_len + 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || !value || strchr(name, '=')) return -1;

    size_t name_len = strlen(name);
    int i = env_find(name, name_len);
    if (i >= 0 && !overwrite) return 0;
    if (i < 0 && environ_count >= MAX_ENV_VARS - 1) return -1;

    char *entry = malloc(name_len + strlen(value) + 2);
    if (!entry) return -1;
    strcpy(entry, name);
    strcat(entry, "=");
    strcat(entry, value);

    if (i >= 0) {
        free(environ_storage[i]);
        environ_storage[i] = entry;
    } else {
        environ_storage[environ_count++] = entry;
        environ_storage[environ_count] = NULL;
    }
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) return -1;

    int i = env_find(name, strlen(name));
    if (i < 0) return 0;

    free(environ_storage[i]);
    for (int j = i; j < environ_count - 1; j++) {
        environ_storage[j] = environ_storage[j + 1];
    }
    environ_count--;
    environ_storage[environ_count] = NULL;
    return 0;
}

// Process control
#define ATEXIT_MAX 32
static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*function)(void)) {
    if (!function || atexit_count >= ATEXIT_MAX) return -1;
    atexit_handlers[atexit_count++] = function;
    return 0;
}

void exit(int status) {
    while (atexit_count > 0) {
        atexit_handlers[--atexit_count]();
    }
    _exit(status);
}

void abort(void) {
    _exit(134);
}

int system(const char *command) {
    if (!command) return 1; // Shell is available

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"/bin/shell.elf", "-c", (char *)command, NULL};
        execve("/bin/shell.elf", argv, environ);
        _exit(127); // execve failed
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        return status;
    }

    return -1; // fork failed
}

// Absolute value functions
int abs(int j) {
    return (j < 0) ? -j : j;
}

long labs(long j) {
    return (j < 0) ? -j : j;
}

long long llabs(long long j) {
    return (j < 0) ? -j : j;
}

// Division functions
div_t div(int numer, int denom) {
    div_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    if (denom == 0) {
        result.quot = 0;
        result.rem = 0;
        return result;
    }
    // 64-bit division on i386 is provided by libgcc (__divdi3, __moddi3).
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}
