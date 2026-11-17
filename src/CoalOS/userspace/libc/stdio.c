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

// Output cursor for vsnprintf: always leaves room for the final NUL.
typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} out_t;

static void out_char(out_t *o, char c) {
    if (o->pos < o->size - 1) o->buf[o->pos++] = c;
}

// Write text (len bytes) padded to width with spaces or zeros.
static void out_field(out_t *o, const char *text, size_t len, size_t width,
                      bool left, bool zero) {
    size_t pad = width > len ? width - len : 0;
    size_t i = 0;

    if (!left && zero && len > 0 && text[0] == '-') {
        out_char(o, '-');   // keep the sign in front of the zeros
        i = 1;
    }
    for (size_t k = 0; !left && k < pad; k++) out_char(o, zero ? '0' : ' ');
    for (; i < len; i++) out_char(o, text[i]);
    for (size_t k = 0; left && k < pad; k++) out_char(o, ' ');
}

int vsnprintf(char *str, size_t size, const char *format, va_list args) {
    if (!str || !format || size == 0) return -1;

    out_t o = { str, size, 0 };
    char temp_buf[64];

    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            out_char(&o, *p);
            continue;
        }
        p++;

        bool left = false, zero = false;
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '0') zero = true;
            else break;
        }
        size_t width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (size_t)(*p++ - '0');
        while (*p == 'l') p++;   // long is 32 bits, same as int
        if (!*p) break;

        const char *text = temp_buf;
        size_t len = 0;

        switch (*p) {
            case 'c':
                temp_buf[0] = (char)va_arg(args, int);
                len = 1;
                zero = false;
                break;
            case 's':
                text = va_arg(args, const char *);
                if (!text) text = "(null)";
                len = strlen(text);
                zero = false;
                break;
            case 'd':
            case 'i':
                len = (size_t)int_to_string(va_arg(args, int), temp_buf);
                break;
            case 'u':
                len = (size_t)uint_to_string(va_arg(args, unsigned int), temp_buf, 10);
                break;
            case 'x':
                len = (size_t)uint_to_string(va_arg(args, unsigned int), temp_buf, 16);
                break;
            case 'X': {
                len = (size_t)uint_to_string(va_arg(args, unsigned int), temp_buf, 16);
                for (size_t i = 0; i < len; i++) {
                    if (temp_buf[i] >= 'a' && temp_buf[i] <= 'f') temp_buf[i] -= 'a' - 'A';
                }
                break;
            }
            case 'p':
                temp_buf[0] = '0';
                temp_buf[1] = 'x';
                len = 2 + (size_t)uint_to_string((unsigned int)(uintptr_t)va_arg(args, void *),
                                                 temp_buf + 2, 16);
                break;
            case '%':
                temp_buf[0] = '%';
                len = 1;
                width = 0;
                break;
            default:   // unknown specifier: print it unchanged
                temp_buf[0] = '%';
                temp_buf[1] = *p;
                len = 2;
                width = 0;
                break;
        }
        out_field(&o, text, len, width, left, zero);
    }

    str[o.pos] = '\0';
    return (int)o.pos;
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
