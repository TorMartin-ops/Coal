/**
 * @file stdio.c
 * @brief Standard I/O library implementation for Coal OS userspace
 */

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/stdarg.h>
#include <libc/unistd.h>

// File descriptors for standard streams
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Buffer size for printf operations
#define PRINTF_BUFFER_SIZE 1024

// System call numbers
#define SYS_WRITE 4
#define SYS_PUTS  7

// System call wrapper
static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int result;
    __asm__ volatile (
        "pushl %%ebx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "movl %1, %%eax\n\t"
        "movl %2, %%ebx\n\t"
        "movl %3, %%ecx\n\t"
        "movl %4, %%edx\n\t"
        "int $0x80\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%ebx\n\t"
        : "=a" (result)
        : "m" (num), "m" (arg1), "m" (arg2), "m" (arg3)
        : "cc", "memory"
    );
    return result;
}

int putchar(int c) {
    char ch = (char)c;
    return (write(STDOUT_FILENO, &ch, 1) == 1) ? c : EOF;
}

int puts(const char *s) {
    if (!s) return EOF;
    
    size_t len = strlen(s);
    if (write(STDOUT_FILENO, s, len) != (ssize_t)len) {
        return EOF;
    }
    if (putchar('\n') == EOF) {
        return EOF;
    }
    return 0;
}

// Helper function to print a string of specific length
bool print(const char* data, size_t length) {
    if (!data || length == 0) return true;
    return write(STDOUT_FILENO, data, length) == (ssize_t)length;
}

// Helper functions for number formatting
static void reverse_string(char *str, int length) {
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

static int int_to_string(int num, char *str, int base) {
    int i = 0;
    bool is_negative = false;
    
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }
    
    if (num < 0 && base == 10) {
        is_negative = true;
        num = -num;
    }
    
    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }
    
    if (is_negative) {
        str[i++] = '-';
    }
    
    str[i] = '\0';
    reverse_string(str, i);
    return i;
}

static int uint_to_string(unsigned int num, char *str, int base) {
    int i = 0;
    
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }
    
    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }
    
    str[i] = '\0';
    reverse_string(str, i);
    return i;
}

int printf(const char* format, ...) {
    if (!format) return -1;
    
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}

int vprintf(const char* format, va_list args) {
    char buffer[PRINTF_BUFFER_SIZE];
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    if (len > 0) {
        write(STDOUT_FILENO, buffer, len);
    }
    return len;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

int vsnprintf(char *str, size_t size, const char *format, va_list args) {
    if (!str || !format || size == 0) return -1;
    
    size_t pos = 0;
    const char *p = format;
    char temp_buf[64];
    
    while (*p && pos < size - 1) {
        if (*p != '%') {
            str[pos++] = *p++;
            continue;
        }
        
        p++; // Skip '%'
        
        // Handle format specifiers
        switch (*p) {
            case 'c': {
                int c = va_arg(args, int);
                if (pos < size - 1) {
                    str[pos++] = (char)c;
                }
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char*);
                if (s) {
                    while (*s && pos < size - 1) {
                        str[pos++] = *s++;
                    }
                } else {
                    const char *null_str = "(null)";
                    while (*null_str && pos < size - 1) {
                        str[pos++] = *null_str++;
                    }
                }
                break;
            }
            case 'd':
            case 'i': {
                int num = va_arg(args, int);
                int len = int_to_string(num, temp_buf, 10);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    str[pos++] = temp_buf[i];
                }
                break;
            }
            case 'u': {
                unsigned int num = va_arg(args, unsigned int);
                int len = uint_to_string(num, temp_buf, 10);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    str[pos++] = temp_buf[i];
                }
                break;
            }
            case 'x': {
                unsigned int num = va_arg(args, unsigned int);
                int len = uint_to_string(num, temp_buf, 16);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    str[pos++] = temp_buf[i];
                }
                break;
            }
            case 'X': {
                unsigned int num = va_arg(args, unsigned int);
                int len = uint_to_string(num, temp_buf, 16);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    char c = temp_buf[i];
                    str[pos++] = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
                }
                break;
            }
            case 'p': {
                void *ptr = va_arg(args, void*);
                if (pos < size - 3) {
                    str[pos++] = '0';
                    str[pos++] = 'x';
                }
                unsigned int addr = (unsigned int)(uintptr_t)ptr;
                int len = uint_to_string(addr, temp_buf, 16);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    str[pos++] = temp_buf[i];
                }
                break;
            }
            case '%': {
                if (pos < size - 1) {
                    str[pos++] = '%';
                }
                break;
            }
            default:
                // Unknown format specifier, just copy it
                if (pos < size - 2) {
                    str[pos++] = '%';
                    str[pos++] = *p;
                }
                break;
        }
        p++;
    }
    
    str[pos] = '\0';
    return (int)pos;
}

int fprintf(FILE *stream, const char *format, ...) {
    // For now, just redirect to stdout/stderr
    // TODO: Implement proper FILE structure support
    (void)stream; // Suppress unused parameter warning
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}

// Minimal FILE structure support (stdout, stderr)
FILE *stdout = (FILE*)1;
FILE *stderr = (FILE*)2;
FILE *stdin = (FILE*)0;