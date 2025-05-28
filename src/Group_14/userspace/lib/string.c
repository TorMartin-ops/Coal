/*
 * Userspace string library implementation
 * Provides basic string and memory functions for userspace programs
 * that are compiled with -nostdlib
 */

#include <libc/stddef.h>

// Memory functions
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n-- > 0) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dest;
}

// String functions
size_t strlen(const char *s) {
    size_t i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

int strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char *strcpy(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while ((*dest++ = *src++));
    return ret;
}

char *strcat(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return ret;
}