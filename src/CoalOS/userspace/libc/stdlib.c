/**
 * @file stdlib.c
 * @brief Standard library functions for Coal OS userspace
 */

#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/limits.h>
#include <libc/stdbool.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

// Simple heap allocator for userspace
static char heap[HEAP_SIZE];
static size_t heap_pos = 0;
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
    if (size == 0) return NULL;
    
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

// String conversion functions
int atoi(const char *str) {
    if (!str) return 0;
    
    int result = 0;
    int sign = 1;
    
    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || 
           *str == '\r' || *str == '\f' || *str == '\v') {
        str++;
    }
    
    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Convert digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

long atol(const char *str) {
    if (!str) return 0;
    
    long result = 0;
    int sign = 1;
    
    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || 
           *str == '\r' || *str == '\f' || *str == '\v') {
        str++;
    }
    
    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Convert digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

long long atoll(const char *str) {
    if (!str) return 0;
    
    long long result = 0;
    int sign = 1;
    
    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || 
           *str == '\r' || *str == '\f' || *str == '\v') {
        str++;
    }
    
    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Convert digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

long strtol(const char *str, char **endptr, int base) {
    if (!str) {
        if (endptr) *endptr = (char*)str;
        return 0;
    }
    
    if (base < 0 || base == 1 || base > 36) {
        if (endptr) *endptr = (char*)str;
        return 0;
    }
    
    const char *start = str;
    long result = 0;
    int sign = 1;
    
    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || 
           *str == '\r' || *str == '\f' || *str == '\v') {
        str++;
    }
    
    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Auto-detect base if base == 0
    if (base == 0) {
        if (*str == '0') {
            if (str[1] == 'x' || str[1] == 'X') {
                base = 16;
                str += 2;
            } else {
                base = 8;
                str++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16 && *str == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    
    // Convert digits
    while (*str) {
        int digit;
        if (*str >= '0' && *str <= '9') {
            digit = *str - '0';
        } else if (*str >= 'a' && *str <= 'z') {
            digit = *str - 'a' + 10;
        } else if (*str >= 'A' && *str <= 'Z') {
            digit = *str - 'A' + 10;
        } else {
            break;
        }
        
        if (digit >= base) break;
        
        result = result * base + digit;
        str++;
    }
    
    if (endptr) {
        *endptr = (char*)(str == start ? start : str);
    }
    
    return result * sign;
}

// Random number generation (simple LCG)
static unsigned long next_rand = 1;

int rand(void) {
    next_rand = next_rand * 1103515245 + 12345;
    return (unsigned int)(next_rand / 65536) % RAND_MAX;
}

void srand(unsigned int seed) {
    next_rand = seed;
}

// Environment variables (simple implementation)
static char *environ_storage[MAX_ENV_VARS];
static int environ_count = 0;

char **environ = environ_storage;

char *getenv(const char *name) {
    if (!name) return NULL;
    
    size_t name_len = strlen(name);
    
    for (int i = 0; i < environ_count; i++) {
        if (environ_storage[i] && 
            strncmp(environ_storage[i], name, name_len) == 0 &&
            environ_storage[i][name_len] == '=') {
            return environ_storage[i] + name_len + 1;
        }
    }
    
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || strchr(name, '=')) return -1;
    
    // Check if variable already exists
    for (int i = 0; i < environ_count; i++) {
        if (environ_storage[i]) {
            size_t name_len = strlen(name);
            if (strncmp(environ_storage[i], name, name_len) == 0 &&
                environ_storage[i][name_len] == '=') {
                
                if (!overwrite) return 0;
                
                // Replace existing
                free(environ_storage[i]);
                size_t total_len = strlen(name) + strlen(value) + 2;
                environ_storage[i] = malloc(total_len);
                if (!environ_storage[i]) return -1;
                
                strcpy(environ_storage[i], name);
                strcat(environ_storage[i], "=");
                strcat(environ_storage[i], value);
                return 0;
            }
        }
    }
    
    // Add new variable
    if (environ_count >= MAX_ENV_VARS - 1) return -1;
    
    size_t total_len = strlen(name) + strlen(value) + 2;
    environ_storage[environ_count] = malloc(total_len);
    if (!environ_storage[environ_count]) return -1;
    
    strcpy(environ_storage[environ_count], name);
    strcat(environ_storage[environ_count], "=");
    strcat(environ_storage[environ_count], value);
    environ_count++;
    environ_storage[environ_count] = NULL;
    
    return 0;
}

int unsetenv(const char *name) {
    if (!name || strchr(name, '=')) return -1;
    
    size_t name_len = strlen(name);
    
    for (int i = 0; i < environ_count; i++) {
        if (environ_storage[i] &&
            strncmp(environ_storage[i], name, name_len) == 0 &&
            environ_storage[i][name_len] == '=') {
            
            free(environ_storage[i]);
            
            // Shift remaining entries
            for (int j = i; j < environ_count - 1; j++) {
                environ_storage[j] = environ_storage[j + 1];
            }
            environ_count--;
            environ_storage[environ_count] = NULL;
            return 0;
        }
    }
    
    return 0;
}

// Process control
void abort(void) {
    // Send SIGABRT to self (when signals are implemented)
    exit(1);
}

int atexit(void (*function)(void)) {
    // TODO: Implement atexit handlers
    return 0;
}

int system(const char *command) {
    if (!command) return 1; // Shell is available
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        char *argv[] = {"/bin/sh", "-c", (char*)command, NULL};
        execve("/bin/sh", argv, environ);
        exit(127); // execve failed
    } else if (pid > 0) {
        // Parent process
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
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}