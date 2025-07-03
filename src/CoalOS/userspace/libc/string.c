/**
 * @file string.c
 * @brief String manipulation functions for Coal OS userspace
 */

#include <libc/string.h>
#include <libc/stdbool.h>

// Memory functions
void *memcpy(void *dest, const void *src, size_t n) {
    if (!dest || !src) return dest;
    
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;
    
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    
    return dest;
}

void *memset(void *s, int c, size_t n) {
    if (!s) return s;
    
    unsigned char *ptr = (unsigned char*)s;
    unsigned char value = (unsigned char)c;
    
    for (size_t i = 0; i < n; i++) {
        ptr[i] = value;
    }
    
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    if (!dest || !src) return dest;
    
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;
    
    if (d == s) {
        return dest;
    }
    
    if (d < s) {
        // Copy forward
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        // Copy backward to handle overlap
        for (size_t i = n; i > 0; i--) {
            d[i-1] = s[i-1];
        }
    }
    
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    
    const unsigned char *p1 = (const unsigned char*)s1;
    const unsigned char *p2 = (const unsigned char*)s2;
    
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    if (!s) return NULL;
    
    const unsigned char *ptr = (const unsigned char*)s;
    unsigned char ch = (unsigned char)c;
    
    for (size_t i = 0; i < n; i++) {
        if (ptr[i] == ch) {
            return (void*)(ptr + i);
        }
    }
    
    return NULL;
}

// String functions
size_t strlen(const char *s) {
    if (!s) return 0;
    
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    
    return len;
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

int strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
        if (s1[i] == '\0') {
            break;
        }
    }
    
    return 0;
}

char *strcpy(char *dest, const char *src) {
    if (!dest || !src) return dest;
    
    char *original_dest = dest;
    
    while ((*dest++ = *src++));
    
    return original_dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    if (!dest || !src) return dest;
    
    char *original_dest = dest;
    size_t i;
    
    // Copy up to n characters
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    
    // Pad with null characters if needed
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    
    return original_dest;
}

char *strcat(char *dest, const char *src) {
    if (!dest || !src) return dest;
    
    char *original_dest = dest;
    
    // Find end of dest string
    while (*dest) {
        dest++;
    }
    
    // Append src to dest
    while ((*dest++ = *src++));
    
    return original_dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    if (!dest || !src) return dest;
    
    char *original_dest = dest;
    
    // Find end of dest string
    while (*dest) {
        dest++;
    }
    
    // Append up to n characters from src
    for (size_t i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    
    // Ensure null termination
    dest[n] = '\0';
    
    return original_dest;
}

char *strchr(const char *s, int c) {
    if (!s) return NULL;
    
    char ch = (char)c;
    
    while (*s) {
        if (*s == ch) {
            return (char*)s;
        }
        s++;
    }
    
    // Check for null terminator match
    if (ch == '\0') {
        return (char*)s;
    }
    
    return NULL;
}

char *strrchr(const char *s, int c) {
    if (!s) return NULL;
    
    char ch = (char)c;
    const char *last_match = NULL;
    
    while (*s) {
        if (*s == ch) {
            last_match = s;
        }
        s++;
    }
    
    // Check for null terminator match
    if (ch == '\0') {
        return (char*)s;
    }
    
    return (char*)last_match;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    
    if (*needle == '\0') {
        return (char*)haystack;
    }
    
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        
        if (*n == '\0') {
            return (char*)haystack;
        }
        
        haystack++;
    }
    
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    if (!s || !accept) return 0;
    
    size_t count = 0;
    
    while (*s) {
        const char *a = accept;
        bool found = false;
        
        while (*a) {
            if (*s == *a) {
                found = true;
                break;
            }
            a++;
        }
        
        if (!found) {
            break;
        }
        
        count++;
        s++;
    }
    
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    if (!s || !reject) return s ? strlen(s) : 0;
    
    size_t count = 0;
    
    while (*s) {
        const char *r = reject;
        
        while (*r) {
            if (*s == *r) {
                return count;
            }
            r++;
        }
        
        count++;
        s++;
    }
    
    return count;
}

char *strtok_r(char *str, const char *delim, char **saveptr);

char *strtok(char *str, const char *delim) {
    static char *saved_ptr = NULL;
    return strtok_r(str, delim, &saved_ptr);
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!delim || !saveptr) return NULL;
    
    if (str) {
        *saveptr = str;
    }
    
    if (!*saveptr) {
        return NULL;
    }
    
    // Skip leading delimiters
    *saveptr += strspn(*saveptr, delim);
    
    if (**saveptr == '\0') {
        *saveptr = NULL;
        return NULL;
    }
    
    char *token_start = *saveptr;
    
    // Find end of token
    *saveptr += strcspn(*saveptr, delim);
    
    if (**saveptr != '\0') {
        **saveptr = '\0';
        (*saveptr)++;
    } else {
        *saveptr = NULL;
    }
    
    return token_start;
}