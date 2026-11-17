/**
 * @file stdio.c
 * @brief Standard I/O library implementation for Coal OS userspace
 */

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/stdarg.h>
#include <libc/unistd.h>
#include <libc/limits.h>

// Buffer size for printf operations
#define PRINTF_BUFFER_SIZE 1024

struct FILE {
    int fd;
};

static FILE stdin_file = { STDIN_FILENO };
static FILE stdout_file = { STDOUT_FILENO };
static FILE stderr_file = { STDERR_FILENO };

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

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

// Write num in the given base into str, returning the length.
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

static int int_to_string(int num, char *str) {
    if (num >= 0) return uint_to_string((unsigned int)num, str, 10);
    // Negate as unsigned so that INT_MIN works.
    str[0] = '-';
    return 1 + uint_to_string(0u - (unsigned int)num, str + 1, 10);
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
        while (*p == 'l') p++; // long is 32 bits, same as int
        if (!*p) break;

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
                int len = int_to_string(num, temp_buf);
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

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsprintf(str, format, args);
    va_end(args);
    return result;
}

int vsprintf(char *str, const char *format, va_list args) {
    return vsnprintf(str, (size_t)INT_MAX, format, args);
}

int fprintf(FILE *stream, const char *format, ...) {
    if (!stream) return -1;

    char buffer[PRINTF_BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len > 0) write(stream->fd, buffer, len);
    return len;
}
