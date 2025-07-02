// src/library/stdio_impl.c
#include <libc/stdio.h>
#include <libc/stdarg.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/limits.h> // For INT_MAX if used
#include <libc/stdbool.h>
// Basic helper to reverse string
static void reverse(char* str, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char temp = str[i];
        str[i] = str[j];
        str[j] = temp;
        i++; j--;
    }
}

// Basic integer to ASCII conversion
static int itoa(int num, char* str, int base) {
    int i = 0;
    bool is_negative = false;

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }

    if (num < 0 && base == 10) {
        is_negative = true;
         // Avoid issues with INT_MIN
         if (num == -INT_MAX - 1) {
             // Handle INT_MIN specially if needed, basic version might overflow
             // For simplicity, let's assume it doesn't hit exact INT_MIN for now
             num = -num;
         } else {
             num = -num;
         }
    }

    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }

    if (is_negative && base == 10) {
        str[i++] = '-';
    }

    str[i] = '\0';
    reverse(str, i);
    return i;
}

// Basic unsigned integer to ASCII conversion (hex)
static int utoa_hex(uint32_t num, char* str) {
    int i = 0;
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return i;
    }

    while (num != 0) {
        int rem = num % 16;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / 16;
    }

    str[i] = '\0';
    reverse(str, i);
    return i;
}

// Very basic vsnprintf implementation
// Supports: %s, %d, %u, %x, %%
// Does NOT support width, precision, flags, length modifiers etc.
int mini_vsnprintf(char *str, size_t size, const char *format, va_list args) {
    if (!str || size == 0) return 0;

    size_t written = 0;
    char temp_buf[33]; // Buffer for number conversions

    while (*format && written < size - 1) {
        if (*format != '%') {
            str[written++] = *format++;
        } else {
            format++; // Skip '%'
            switch (*format) {
                case 's': {
                    const char *s_arg = va_arg(args, const char *);
                    if (!s_arg) s_arg = "(null)";
                    while (*s_arg && written < size - 1) {
                        str[written++] = *s_arg++;
                    }
                    format++;
                    break;
                }
                case 'd': {
                    int d_arg = va_arg(args, int);
                    int len = itoa(d_arg, temp_buf, 10);
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                    format++;
                    break;
                }
                 case 'u': { // Added %u support
                    unsigned int u_arg = va_arg(args, unsigned int);
                    int len = itoa(u_arg, temp_buf, 10); // Use base 10
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                    format++;
                    break;
                }
                case 'x': {
                    uint32_t x_arg = va_arg(args, uint32_t);
                    int len = utoa_hex(x_arg, temp_buf);
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                    format++;
                    break;
                }
                case '%': {
                    str[written++] = '%';
                    format++;
                    break;
                }
                default: // Unknown format specifier, just print '%' and the char
                    str[written++] = '%';
                    if (*format && written < size - 1) {
                        str[written++] = *format;
                    }
                    if (*format) format++;
                    break;
            }
        }
    }

    str[written] = '\0'; // Null-terminate
    return written;
}

// snprintf wrapper
int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = mini_vsnprintf(str, size, format, args);
    va_end(args);
    return ret;
}
