/**
 * @file string_utils.c
 * @brief String utility functions implementation for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#include "string_utils.h"

//============================================================================
// String Function Implementations
//============================================================================

size_t my_strlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

int my_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int my_strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (n-- && *s1 && (*s1 == *s2)) { s1++; s2++; }
    if (n == (size_t)-1) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char *my_strcpy(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while ((*dest++ = *src++));
    return ret;
}

char *my_strncpy(char *dest, const char *src, size_t n) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while (n-- && (*dest++ = *src++));
    while (n-- > 0) *dest++ = '\0';
    return ret;
}

char *my_strcat(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return ret;
}

char *my_strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

void *my_memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char *trim_whitespace(char *str) {
    if (!str) return NULL;
    
    // Trim leading whitespace
    while (is_whitespace(*str)) str++;
    
    // If all spaces or empty string
    if (*str == '\0') return str;
    
    // Trim trailing whitespace
    char *end = str + my_strlen(str) - 1;
    while (end > str && is_whitespace(*end)) end--;
    
    // Null terminate
    *(end + 1) = '\0';
    
    return str;
}